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
 * A BRICK THAT LOST ONE OF ITS TWO BED SEATS AND STILL ABUTS A NEIGHBOUR THAT REACHES THE GROUND
 * ON ITS OWN ARCHES OVER THE HOLE — it does not cantilever over it.
 *
 * THE DEFECT, AS REPORTED IN THE GAME. Delete one brick and the failure walks across the wall in
 * a stepping triangle, one step per course. The mechanism, worked through:
 *
 * In running bond a brick rests on TWO bed patches at -/+5.625 cm, each 105.0625 cm2. Their
 * area-weighted centroid coincides with the centre of mass, so e = 0 EXACTLY — MOMENTS_DESIGN's
 * N >= 2 rule drops the moment, and that is where the intact wall's 0.005 comes from. Delete one
 * brick beneath and the brick above has EXACTLY ONE seat. That is N = 1, statically determinate,
 * so the moment IS computed, and three things worsen at once: the area halves, the section
 * modulus falls by four as the depth halves, and e goes from exactly zero to 5.625 cm. Per brick
 * weight carried the joint reads 0.058203838 of f_xk1, so it reaches 1.0 at 17.18 brick weights —
 * and a brick in course 2 of a 30-course wall carries 28 of them.
 *
 * WHAT IS PHYSICALLY WRONG WITH THAT. A cantilever is what you get when there is nothing on the
 * overhanging side to push against. Here there IS: an intact head joint to a brick that has its
 * own seat on the course below, so the two lean on each other and the thrust line runs through
 * the hole instead of peeling the seat open. An arch is exactly a load path that cannot develop
 * tension because the thrust line stays inside the section, and the section's limit for that is
 * the kern.
 *
 * THE RULE BEING ASSERTED, from ARCHING_DESIGN.md. A joint develops arching relief when ALL of:
 * it is a BedBeneath joint of the loaded piece and the piece has COMPLETE GEOMETRY; the normal
 * force is COMPRESSIVE; the load resultant is eccentric BEYOND THE KERN (+/- 1.7083 cm on a
 * 10.25 cm deep patch); and there is an INTACT HEAD JOINT ON THE ECCENTRIC SIDE to a neighbour
 * that is Supported or Grounded and does not count this piece among its own supports. The relief
 * is a cap on the moment VECTOR, k = min(1, |sigma_n|/sigma_b), so at the cap peak tension is
 * exactly zero and peak compression is exactly 2|sigma_n|.
 *
 * THE ASSERTION IS ON THE MECHANISM, TWICE OVER, AND NEVER ON DISPLACEMENT. Nothing here moves:
 * this is a utilisation ratio on a named joint, plus a count of joints that gave. Two pieces can
 * sever and stay resting exactly where they were, so how far anything travelled would say
 * nothing about whether the mortar held.
 *
 * WHAT MAKES THE POSITIVE ROW HARD TO GET WRONG. The capped answer is asserted TWICE and neither
 * form is the design document's arithmetic:
 *
 *   - against 2|sigma_n|/f_c recomputed from the force THE SOLVER REPORTS, which is the physical
 *     statement "the thrust line sits on the kern edge" and holds whatever the wall's real load
 *     distribution turns out to be;
 *   - against the flat literal 0.0142166 that ARCHING_DESIGN.md predicts from 28 brick weights,
 *     with enough slack for the wall's true distribution not to be exactly 28 and not a drop
 *     more. If those two disagree the design's arithmetic is what is wrong, and it fails HERE
 *     rather than being tuned away.
 *
 * AND THE "BEFORE" NUMBER IS BUILT FROM THE FIXTURE, NEVER FROM THE SOLVER'S PUBLISHED MOMENT.
 * The design requires the CAPPED moment to be both what travels and what the joint publishes, so
 * after the arch fires GetConnectionMoment cannot state the cantilever answer and any oracle
 * standing on it returns the arched one. Every row that wants the un-arched figure therefore
 * rebuilds it here — the share the joint reports carrying, through the 5.625 cm the brick
 * overhangs its patch, against that patch's 179.4817708 cm3 section. Only the FORCE is taken from
 * the solver, and the cap does not touch a force.
 *
 * WHICH AXIS GOVERNS IS ASSERTED BEFORE ANYTHING IS CLAIMED. ComputeUtilisation returns the WORST
 * of compression, shear and tension, so a fixture aimed at bending silently measures compression
 * the moment compression is higher. Before the cap tension governs by a wide margin; AFTER it,
 * compression governs and tension is exactly zero — the axis DELIBERATELY changes hands, which is
 * what an arch is, so both sides are worked out and printed rather than assumed. Shear is exactly
 * zero throughout: gravity is normal to a bed joint.
 *
 * THE FOUR GATES EACH GET A NEGATIVE ROW, and they are as much the point as the new number is. An
 * arch that fires one gate too eagerly is worse than no arch at all, because it makes things
 * unbreakable and unbreakable is invisible. Every row asserts the joint reads exactly what
 * ordinary beam theory says it reads, recomputed here from the solver's own force and moment, so
 * a row stays meaningful without anyone re-measuring a literal.
 *
 * AND FOUR REGRESSION ANCHORS, EACH ALREADY OWNED BY ANOTHER TEST. They are re-asserted here
 * because they are the arithmetic this slice is most likely to destroy, and a failure that names
 * the arch in its own test is worth more than one that appears three files away. The staircase in
 * particular is the DIRECTION CHECK: the corbel's eccentric side is where the cut removed the
 * neighbour, so there is no head joint there and the arch must be refused. Apply the cap without
 * checking the direction and the staircase falls over on its own — the suite already contains its
 * own guard, and this keeps that visible.
 *
 * NEEDS A TICKING WORLD: NO. Not one line of this needs an actor, a tick or a renderer. FStructure
 * is plain arithmetic over a graph and Layout is plain arithmetic over boxes; the 980 is the
 * solver's own constant, not a physics scene's.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureArchingTest,
	"DestructionGame.Core.Structure.AMissingBrickIsBridgedNotCantilevered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureArchingTest::RunTest(const FString& Parameters)
{
	using namespace StructureArchingTestSupport;
	using namespace StaircaseWallTestSupport;

	/*
	 * THE EXPECTED NUMBERS ARE RATIOS OF PUBLISHED STRENGTHS, so they mean what they say only
	 * while the profile still carries the figures they were derived against. Asserted rather
	 * than imported: a test that read the profile would agree with a wrong profile.
	 */
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against f_xk1 = 0.1 MPa, the profile carries %g"),
			GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == 0.1);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against compressive 10 MPa, the profile carries %g"),
			GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == 10.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against cohesion 0.2 MPa, the profile carries %g"),
			GeneralPurposeMortar.ShearCohesionMPa),
		GeneralPurposeMortar.ShearCohesionMPa == 0.2);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against friction 0.6, the profile carries %g"),
			GeneralPurposeMortar.FrictionCoefficient),
		GeneralPurposeMortar.FrictionCoefficient == 0.6);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == 1.9);

	/*
	 * ===================================================================================
	 * PART 1 — THE INTACT WALL, WHICH MUST NOT MOVE BY ONE BIT.
	 * ===================================================================================
	 */

	/*
	 * ASSERTED WITH == AND NOT WITH A TOLERANCE, and the reason is measured rather than
	 * fastidious: the cascade fuzz has five joints settling at exactly 1.0 with one at
	 * 1 - 1ulp, so a last-bit drift in the intact wall's arithmetic is the difference between
	 * a joint that holds and a joint that gives. This is also the STRONGEST anchor available
	 * for this slice — every one of the four gates is false everywhere in an intact wall, so
	 * the new code must be UNREACHED here rather than merely inert, and only exact equality
	 * can tell those two apart.
	 */
	FBrickLayout Intact;

	if (!RunningBond(ArchWallSpec(), Intact) || Intact.Boxes.Num() != ArchWallPieceCount)
	{
		AddError(FString::Printf(
			TEXT("FIXTURE: a flush 7 x 30 wall should lay as %d pieces, got %d"),
			ArchWallPieceCount, Intact.Boxes.Num()));

		return true;
	}

	TestTrue(
		TEXT("FIXTURE: the laid wall must know where every piece and every joint is, or every "
			 "moment below is silently zero and this measures nothing"),
		Intact.Structure.HasCompleteGeometry());

	Intact.Structure.SolveLoads();

	double IntactWorst = 0.0;
	int32 IntactWorstJoint = INDEX_NONE;

	for (int32 Joint = 0; Joint < Intact.Structure.NumConnections(); ++Joint)
	{
		const double Utilisation = Intact.Structure.GetConnectionUtilisation(Joint);

		if (Utilisation > IntactWorst)
		{
			IntactWorst = Utilisation;
			IntactWorstJoint = Joint;
		}
	}

	AddInfo(FString::Printf(
		TEXT("ANCHOR 1: intact 7 x 30 flush wall, %d pieces and %d joints, worst joint %d at %s"),
		Intact.Structure.NumPieces(), Intact.Structure.NumConnections(),
		IntactWorstJoint, *Bits(IntactWorst)));

	/**
	 * Measured, and pinned bit for bit. See the note above on why this is == and not ~=.
	 *
	 * NOT ARCHING_DESIGN's 0.00495, and the difference is the wall rather than the model: that
	 * figure is the 40-course scenario wall's, and a 30-course wall of the same bond carries
	 * proportionally less. Both are the same statement — every seat of an intact wall has
	 * e = 0 exactly, so nothing anywhere in it carries a moment.
	 */
	constexpr double IntactWallWorstUtilisation = 0.0036748258197270385;

	TestTrue(
		FString::Printf(
			TEXT("ANCHOR 1: the intact wall's worst joint must be BIT-IDENTICAL at %s, it reads %s"),
			*Bits(IntactWallWorstUtilisation), *Bits(IntactWorst)),
		IntactWorst == IntactWallWorstUtilisation);

	TestTrue(
		FString::Printf(TEXT("ANCHOR 1: and it must be nowhere near capacity, it reads %s"),
			*Bits(IntactWorst)),
		IntactWorst < 0.01);

	/*
	 * ===================================================================================
	 * PART 2 — ONE INTERIOR DELETION, AND THE TWO JOINTS IT LEAVES HALF-SEATED.
	 * ===================================================================================
	 */

	FBrickLayout Cut;

	if (!RunningBond(ArchWallSpec(), Cut) || Cut.Boxes.Num() != ArchWallPieceCount)
	{
		AddError(TEXT("FIXTURE: the second copy of the wall did not lay"));
		return true;
	}

	const int32 DeletedPiece = StaircasePieceAt(
		Cut.Boxes, DeletedBrickXCm, ArchWallCourseZCm(DeletedBrickCourse));

	if (DeletedPiece == INDEX_NONE || !Cut.Structure.RemovePiece(DeletedPiece))
	{
		AddError(FString::Printf(
			TEXT("FIXTURE: there should be a brick at x %.2f in course %d to delete"),
			DeletedBrickXCm, DeletedBrickCourse));

		return true;
	}

	Cut.Structure.SolveLoads();

	/**
	 * One of the two bricks the deletion left standing on half a seat, and everything about
	 * the joint under it that the arching rule has an opinion on.
	 */
	struct FHalfSeatedCase
	{
		const TCHAR* Description;

		/** Where the half-seated brick is, and where the seat it kept is. */
		double BrickXCm;
		double SeatXCm;

		/** Which way it overhangs: +1 is toward increasing X. The ECCENTRIC side. */
		double EccentricSign;

		/** Where its abutment is — the brick it leans on through the head joint. */
		double AbutmentXCm;
	};

	const TArray<FHalfSeatedCase> HalfSeated = {
		{
			TEXT("the brick on the LEFT of the hole, overhanging RIGHT into it"),
			LeftHalfSeatedXCm, LeftSurvivingSeatXCm, +1.0, RightHalfSeatedXCm
		},
		{
			TEXT("the brick on the RIGHT of the hole, overhanging LEFT into it"),
			RightHalfSeatedXCm, RightSurvivingSeatXCm, -1.0, LeftHalfSeatedXCm
		},
	};

	/**
	 * ARCHING_DESIGN.md's own prediction, from 28 brick weights on one 105.0625 cm2 patch.
	 *
	 * 2 * (28 * 2667.198625 / (105.0625 * 10000)) / 10 MPa = 0.0142166, against the 1.62971
	 * the same 28 brick weights read as a cantilever — a ratio of 114.63, and load-independent
	 * because both terms are linear in the load.
	 *
	 * THE SLACK IS FOR THE WALL'S REAL LOAD DISTRIBUTION, NOT FOR THE PHYSICS. "About N - j
	 * brick weights" is an idealisation: the wall's odd courses weigh slightly less than its
	 * even ones because two half bats are lighter than one full brick, and the deletion itself
	 * moves load sideways. Two percent covers that and covers nothing else — a wrong lever
	 * arm, a wrong section modulus, a missing 100x or the cap implemented as M = 0 (which is
	 * out by exactly a factor of two) are all far outside it. The TIGHT statement of the same
	 * claim is the 2|sigma_n| identity asserted beside this one.
	 */
	constexpr double PredictedArchedUtilisation = 0.0142166;
	constexpr double PredictedArchedTolerance = 0.02;

	/**
	 * And what the same joint would read WITHOUT the arch, for the same 28 brick weights.
	 *
	 * Asserted against a cantilever reading rebuilt from the fixture's own 5.625 cm arm — see
	 * the derivation at the assertion. NOT against GetConnectionMoment, which by design carries
	 * the capped value once the arch has fired and so cannot state this number at all.
	 */
	constexpr double PredictedCantileverUtilisation = 1.62971;

	for (const FHalfSeatedCase& Case : HalfSeated)
	{
		const int32 Brick = StaircasePieceAt(
			Cut.Boxes, Case.BrickXCm, ArchWallCourseZCm(HalfSeatedCourse));
		const int32 Seat = StaircasePieceAt(
			Cut.Boxes, Case.SeatXCm, ArchWallCourseZCm(HalfSeatedCourse - 1));
		const int32 Abutment = StaircasePieceAt(
			Cut.Boxes, Case.AbutmentXCm, ArchWallCourseZCm(HalfSeatedCourse));

		if (Brick == INDEX_NONE || Seat == INDEX_NONE || Abutment == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("FIXTURE: %s — brick %d, seat %d, abutment %d"),
				Case.Description, Brick, Seat, Abutment));

			continue;
		}

		/*
		 * GATE ONE, AS A FIXTURE PRECONDITION: exactly one bed joint beneath. N = 1 is what
		 * makes the moment determinate and computed at all; if the deletion had left two the
		 * moment would be dropped as indeterminate and this fixture would be measuring the
		 * intact wall with extra steps.
		 *
		 * JOINTS THAT HAVE GIVEN ARE SKIPPED, AND FORGETTING TO IS A TRAP THIS FIXTURE FELL IN
		 * ONCE. GetJointRole is pure geometry and keeps answering for a joint that has left the
		 * structure — deliberately, because what a given joint USED to be is exactly what a
		 * debugger is looking at — so the joint that went with the deleted brick still reports
		 * BedBeneath and a naive count says two. The support relation is what N means, and a
		 * given joint is not in it.
		 */
		int32 BedBeneath = INDEX_NONE;
		int32 BedBeneathCount = 0;

		for (int32 Joint = 0; Joint < Cut.Structure.NumConnections(); ++Joint)
		{
			if (Cut.Structure.GetJointRole(Joint, Brick) == EJointRole::BedBeneath
				&& !Cut.Structure.GetConnection(Joint).HasGiven())
			{
				BedBeneath = Joint;
				++BedBeneathCount;
			}
		}

		TestEqual(
			FString::Printf(
				TEXT("%s: FIXTURE: it must rest on EXACTLY ONE bed joint after the deletion, got %d"),
				Case.Description, BedBeneathCount),
			BedBeneathCount, 1);

		if (BedBeneathCount != 1 || BedBeneath != JointBetweenPieces(Cut.Structure, Brick, Seat))
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE: the one bed joint beneath should be the one to piece %d"),
				Case.Description, Seat));

			continue;
		}

		const FConnection& Bed = Cut.Structure.GetConnection(BedBeneath);

		/*
		 * GATE ONE AGAIN, THE OTHER HALF: the joint knows its own rectangle. The half seat is
		 * the 10.25 x 10.25 strip the two still share, and every lever arm and every section
		 * modulus below is measured from it — so it is arbitrated against the producer rather
		 * than assumed, and the number and the reason for it fail together.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the surviving seat should be %g cm2 with half-extents (%g, %g, 0); ")
				TEXT("MakeInterface emitted %g cm2 with (%g, %g, %g)"),
				Case.Description, HalfSeatAreaSqCm,
				HalfSeatHalfExtentCm, HalfSeatHalfExtentCm,
				Bed.InterfaceAreaSqCm,
				Bed.InterfaceHalfExtentCm.X, Bed.InterfaceHalfExtentCm.Y, Bed.InterfaceHalfExtentCm.Z),
			FMath::IsNearlyEqual(Bed.InterfaceAreaSqCm, HalfSeatAreaSqCm, 1.0e-9)
				&& FMath::IsNearlyEqual(Bed.InterfaceHalfExtentCm.X, HalfSeatHalfExtentCm, 1.0e-9)
				&& FMath::IsNearlyEqual(Bed.InterfaceHalfExtentCm.Y, HalfSeatHalfExtentCm, 1.0e-9)
				&& Bed.InterfaceHalfExtentCm.Z == 0.0);

		/*
		 * GATE THREE: the load path is eccentric BEYOND THE KERN, and it is on the side the
		 * fixture says it is. The brick's centre of mass sits 5.625 cm from the patch's
		 * centroid on a patch whose kern reaches 1.7083 cm, so part of the face is opening —
		 * which is the only condition under which an arch has anything to relieve.
		 */
		const double EccentricityCm =
			Cut.Boxes[Brick].CentreCm.X - Bed.InterfaceCentreCm.X;

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: it should overhang its seat by %g cm toward %s, it overhangs %g"),
				Case.Description, HalfSeatEccentricityCm,
				Case.EccentricSign > 0.0 ? TEXT("+X") : TEXT("-X"), EccentricityCm),
			FMath::IsNearlyEqual(EccentricityCm, Case.EccentricSign * HalfSeatEccentricityCm, 1.0e-9));

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: %g cm of eccentricity must be OUTSIDE the kern's %g cm, or ")
				TEXT("no part of the face is opening and there is nothing to relieve"),
				Case.Description, FMath::Abs(EccentricityCm),
				KernFromHalfExtentCm(HalfSeatHalfExtentCm)),
			FMath::Abs(EccentricityCm) > KernFromHalfExtentCm(HalfSeatHalfExtentCm));

		/*
		 * GATE FOUR: an intact head joint on the ECCENTRIC side, to a neighbour that reaches
		 * the ground on its own account.
		 *
		 * "ON ITS OWN ACCOUNT" IS THE WHOLE OF TRAP 3 AND IS CHECKED, NOT ASSUMED. Two unseated
		 * bricks propping each other look locally identical to a real arch — each is Supported,
		 * each has an intact head joint to the other — and granting them an arch would let a
		 * wall hang from nothing. What separates this case is that the abutment has a BED joint
		 * of its own, to a piece that is not the one asking. Checked here so that a fixture
		 * which quietly stopped being an arch would fail as a fixture rather than as physics.
		 */
		const int32 HeadJoint = JointBetweenPieces(Cut.Structure, Brick, Abutment);

		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: there must be a head joint to piece %d"),
				Case.Description, Abutment),
			HeadJoint != INDEX_NONE
				&& Cut.Structure.GetJointRole(HeadJoint, Brick) == EJointRole::Head
				&& !Cut.Structure.GetConnection(HeadJoint).HasGiven());

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the abutment must lie on the ECCENTRIC side — brick at x %g, ")
				TEXT("abutment at x %g"),
				Case.Description, Cut.Boxes[Brick].CentreCm.X, Cut.Boxes[Abutment].CentreCm.X),
			(Cut.Boxes[Abutment].CentreCm.X - Cut.Boxes[Brick].CentreCm.X) * Case.EccentricSign > 0.0);

		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: the abutment must itself reach the ground"),
				Case.Description),
			Cut.Structure.GetPieceSupport(Abutment) == EPieceSupport::Supported
				|| Cut.Structure.GetPieceSupport(Abutment) == EPieceSupport::Grounded);

		int32 AbutmentSeats = 0;
		bool bAbutmentLeansOnUs = false;

		for (int32 Joint = 0; Joint < Cut.Structure.NumConnections(); ++Joint)
		{
			if (Cut.Structure.GetJointRole(Joint, Abutment) != EJointRole::BedBeneath
				|| Cut.Structure.GetConnection(Joint).HasGiven())
			{
				continue;
			}

			++AbutmentSeats;

			const FConnection& Other = Cut.Structure.GetConnection(Joint);

			if (Other.PieceA == Brick || Other.PieceB == Brick)
			{
				bAbutmentLeansOnUs = true;
			}
		}

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the abutment must have a seat of its own (it has %d) and must ")
				TEXT("NOT be resting on the piece asking for the arch (it %s)"),
				Case.Description, AbutmentSeats,
				bAbutmentLeansOnUs ? TEXT("is") : TEXT("is not")),
			AbutmentSeats >= 1 && !bAbutmentLeansOnUs);

		/*
		 * AND THE PIECE ITSELF IS STILL ROUTED. A half-seated brick reported Stranded would
		 * make every number below a solver limitation wearing a collapse's clothes.
		 */
		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: the half-seated brick must still be Supported"),
				Case.Description),
			Cut.Structure.GetPieceSupport(Brick) == EPieceSupport::Supported);

		// --- what the joint actually reads, and what it must read --------------------------

		const FVector ForceUu = Cut.Structure.GetConnectionForce(BedBeneath);
		const FVector MomentUuCm = Cut.Structure.GetConnectionMoment(BedBeneath);

		const double Utilisation = Cut.Structure.GetConnectionUtilisation(BedBeneath);

		/*
		 * WHAT THE JOINT PUBLISHES, read back through beam theory. The moment handed in is the
		 * solver's own, so this answers "is the reading self-consistent with the moment the
		 * joint says it carries" — which is the right question for a negative row and, after
		 * the cap, a question that can only ever have one answer.
		 */
		const FBedJointReading Published = ReadBedJoint(
			ForceUu, MomentUuCm, Bed.InterfaceHalfExtentCm, Bed.InterfaceAreaSqCm,
			GeneralPurposeMortar);

		/*
		 * AND WHAT IT WOULD HAVE READ AS A CANTILEVER — BUILT FROM THE FIXTURE'S OWN LEVER ARM,
		 * NEVER FROM THE SOLVER'S PUBLISHED MOMENT, AND THAT DISTINCTION IS THE WHOLE ROW.
		 *
		 * ARCHING_DESIGN requires the capped moment to be BOTH what travels and what the joint
		 * publishes — Structure.cpp's own "what travels is what the joint reads, there is no
		 * second, private quantity" — so once the arch fires, GetConnectionMoment returns the
		 * relieved 127329.05 uu.cm and any "before" figure derived from it is the arched answer
		 * wearing the cantilever's name. An earlier draft of these three rows did exactly that
		 * and could not pass at the same time as the claim they precede.
		 *
		 * So the before-number is re-derived here instead: the whole share the joint reports
		 * carrying, acting through the 5.625 cm a half-seated running-bond brick overhangs the
		 * patch it kept, against that patch's 179.4817708 cm3 section. Only the FORCE comes from
		 * the solver, and the cap does not touch a force. Eccentricity is along X so the moment
		 * is about Y; the patch is square, so the two in-plane moduli are equal anyway and the
		 * axis choice cannot hide an error here — it is written correctly because a fixture on a
		 * brick's END face would have them 2.5x apart.
		 *
		 * IT IS AN IDEALISATION BY ABOUT A TENTH OF A PERCENT, and knowingly. The true uncapped
		 * moment also carries whatever arrived from the courses above, which very nearly cancels
		 * across the two symmetric seats up there but not exactly; the right-hand joint comes out
		 * at 1.62718 against the 1.62719 the uncapped solver measured, and the left-hand one at
		 * 1.62648 against 1.62749. Both are inside the 2% the design's own literal is asserted
		 * to, which is the only claim made on them.
		 */
		const FVector CantileverMomentUuCm(
			0.0, FMath::Abs(ForceUu.Z) * HalfSeatEccentricityCm, 0.0);

		const FBedJointReading Cantilever = ReadBedJoint(
			ForceUu, CantileverMomentUuCm, Bed.InterfaceHalfExtentCm, Bed.InterfaceAreaSqCm,
			GeneralPurposeMortar);

		const double Arched = ArchedUtilisation(Published, GeneralPurposeMortar);

		AddInfo(FString::Printf(
			TEXT("%s: piece %3d on %3d through joint %3d carries %s uu (%.4f brick weights) and ")
			TEXT("publishes %s uu.cm; sigma_n %s MPa, sigma_b %s MPa (a cantilever's %s uu.cm ")
			TEXT("would be sigma_b %s MPa)"),
			Case.Description, Brick, Seat, BedBeneath,
			*Bits(ForceUu.Size()), ForceUu.Size() / BrickWeightUu,
			*Bits(MomentUuCm.Size()),
			*Bits(Published.NormalStressMPa), *Bits(Published.BendingStressMPa),
			*Bits(CantileverMomentUuCm.Size()), *Bits(Cantilever.BendingStressMPa)));

		AddInfo(FString::Printf(
			TEXT("%s: reads %s; as a cantilever %s (tension %s, compression %s, shear %s); ")
			TEXT("arched %s"),
			Case.Description, *Bits(Utilisation), *Bits(Cantilever.Worst),
			*Bits(Cantilever.TensionUtilisation), *Bits(Cantilever.CompressionUtilisation),
			*Bits(Cantilever.ShearUtilisation), *Bits(Arched)));

		/*
		 * GATE TWO: the normal force is COMPRESSIVE. No compression, no thrust line, no arch —
		 * and on a bed joint under gravity there had better be some, or the fixture is not
		 * describing a wall at all.
		 */
		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: the seat must be in COMPRESSION, sigma_n is %s MPa"),
				Case.Description, *Bits(Published.NormalStressMPa)),
			Published.NormalStressMPa < 0.0);

		/*
		 * AND THE AXIS. Shear is exactly zero — gravity is normal to a bed joint — so the
		 * contest is between tension and compression, and the cap DELIBERATELY hands it over:
		 * tension governs before, compression governs after. Saying so here is what stops a
		 * green run from being a run that measured the wrong axis.
		 */
		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: shear must be exactly zero, it is %s"),
				Case.Description, *Bits(Published.ShearUtilisation)),
			Published.ShearUtilisation == 0.0);

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: as a cantilever TENSION must govern (%s against compression %s), ")
				TEXT("or the change this test asserts is invisible"),
				Case.Description, *Bits(Cantilever.TensionUtilisation),
				*Bits(Cantilever.CompressionUtilisation)),
			Cantilever.TensionUtilisation > Cantilever.CompressionUtilisation);

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the cantilever answer must be over capacity (%s) or nothing was ")
				TEXT("wrong in the first place"),
				Case.Description, *Bits(Cantilever.Worst)),
			Cantilever.Worst > 1.0);

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: ARCHING_DESIGN predicts the cantilever reads %g; beam theory on ")
				TEXT("the solver's own force through the fixture's own 5.625 cm arm says %s"),
				Case.Description, PredictedCantileverUtilisation, *Bits(Cantilever.Worst)),
			FMath::Abs(Cantilever.Worst - PredictedCantileverUtilisation)
				<= PredictedArchedTolerance * PredictedCantileverUtilisation);

		/* THE CLAIM, tightly: the thrust line sits on the kern edge and nothing else changed. */
		TestTrue(
			FString::Printf(
				TEXT("%s: the half-seated joint must ARCH — 2|sigma_n|/f_c = %s — and it reads %s ")
				TEXT("(the cantilever answer is %s)"),
				Case.Description, *Bits(Arched), *Bits(Utilisation), *Bits(Cantilever.Worst)),
			FMath::Abs(Utilisation - Arched) <= 1.0e-12 * Arched);

		/*
		 * AND THE CAPPED MOMENT IS THE ONE THE JOINT PUBLISHES, which is why the cantilever
		 * oracle above had to be re-derived rather than read back off GetConnectionMoment.
		 * ARCHING_DESIGN puts the thrust line exactly on the kern edge, and the kern edge is
		 * the definition sigma_b == |sigma_n| — so a solver that relieved the stress it
		 * EVALUATES while handing an unrelieved moment down to the course below would keep the
		 * row above green and fail here. That is the second, private quantity Structure.cpp's
		 * "what travels is what the joint reads" exists to forbid.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: the moment it PUBLISHES must sit on the kern edge — sigma_b %s against ")
				TEXT("|sigma_n| %s"),
				Case.Description, *Bits(Published.BendingStressMPa),
				*Bits(FMath::Abs(Published.NormalStressMPa))),
			FMath::Abs(Published.BendingStressMPa - FMath::Abs(Published.NormalStressMPa))
				<= 1.0e-12 * FMath::Abs(Published.NormalStressMPa));

		/* And loosely, against the number the design document derives independently. */
		TestTrue(
			FString::Printf(
				TEXT("%s: ARCHING_DESIGN predicts %g for this joint, it reads %s"),
				Case.Description, PredictedArchedUtilisation, *Bits(Utilisation)),
			FMath::Abs(Utilisation - PredictedArchedUtilisation)
				<= PredictedArchedTolerance * PredictedArchedUtilisation);

		/*
		 * AT THE CAP THE OPENED EDGE CARRIES EXACTLY NOTHING, which is what an arch IS. Stated
		 * separately from the utilisation because a joint reading 0.0142 could also be a joint
		 * whose moment was deleted outright — and that answer is wrong by exactly a factor of
		 * two on the compression axis, |sigma_n| against 2|sigma_n|. The pair of assertions
		 * pins which of the two happened.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: and it must NOT be the moment simply zeroed — that reads %s, half of ")
				TEXT("the %s an arch reads"),
				Case.Description,
				*Bits(FMath::Abs(Published.NormalStressMPa)
					/ GeneralPurposeMortar.CompressiveStrengthMPa),
				*Bits(Arched)),
			FMath::Abs(Utilisation
				- FMath::Abs(Published.NormalStressMPa)
					/ GeneralPurposeMortar.CompressiveStrengthMPa)
				> 1.0e-6 * Arched);
	}

	/*
	 * AND THE WALL STANDS. A single severed joint is not a collapse and a wall can sever a joint
	 * and still stand, so the outcome claim is the count: cutting one interior brick out of a
	 * 30-course wall must break NOTHING, rather than starting the stepping triangle that walks
	 * one step per course and takes the upper half of the wall.
	 *
	 * SolveLoads is non-destructive by contract, so everything read above is still intact and
	 * this can run on the same structure.
	 *
	 * COUNTED BY BREAK PASS AND NOT BY HasGiven, AND THE TWO ARE GENUINELY DIFFERENT HERE.
	 * GetBreakPass's contract spells the encoding out: a joint that went WITH A REMOVED PIECE
	 * has HasGiven true and a pass of INDEX_NONE, because it never snapped — it was deleted
	 * along with the brick it held. The player's own click always takes six joints with it, so
	 * a HasGiven count can never reach zero and would make this assertion unsatisfiable no
	 * matter how correct the physics got. What "the wall stood" means is that nothing FAILED
	 * UNDER LOAD, which is exactly the set phase 5 replays as a collapse.
	 */
	const int32 BreakingPasses = Cut.Structure.SolveAndBreak();

	int32 JointsBrokenByLoad = 0;
	int32 JointsGoneWithTheBrick = 0;

	for (int32 Joint = 0; Joint < Cut.Structure.NumConnections(); ++Joint)
	{
		if (!Cut.Structure.GetConnection(Joint).HasGiven())
		{
			continue;
		}

		if (Cut.Structure.GetBreakPass(Joint) != INDEX_NONE)
		{
			++JointsBrokenByLoad;
		}
		else
		{
			++JointsGoneWithTheBrick;
		}
	}

	AddInfo(FString::Printf(
		TEXT("after one interior deletion the cascade ran %d passes; %d of %d joints failed under ")
		TEXT("load and %d went with the deleted brick"),
		BreakingPasses, JointsBrokenByLoad, Cut.Structure.NumConnections(), JointsGoneWithTheBrick));

	TestEqual(
		FString::Printf(
			TEXT("one interior deletion must break NOTHING; the cascade ran %d passes"),
			BreakingPasses),
		BreakingPasses, 0);

	TestEqual(
		FString::Printf(
			TEXT("one interior deletion must leave every joint it did not delete intact; %d failed ")
			TEXT("under load"),
			JointsBrokenByLoad),
		JointsBrokenByLoad, 0);

	/*
	 * AND THE SIX THE CLICK ITSELF TOOK ARE THE ONLY OTHER CASUALTIES: two seats beneath the
	 * deleted brick, two above it and two head joints along its own course. Pinned so that a
	 * cascade which happened to break exactly zero joints while the deletion quietly took a
	 * seventh could not read as a wall that stood.
	 */
	TestEqual(
		FString::Printf(
			TEXT("only the deleted brick's own six joints may leave the graph; %d did"),
			JointsGoneWithTheBrick),
		JointsGoneWithTheBrick, 6);

	/*
	 * ===================================================================================
	 * PART 3 — THE FOUR GATES, EACH WITH A CASE THAT MUST NOT ARCH.
	 * ===================================================================================
	 */

	/**
	 * A hand-built fixture aimed at exactly one gate, and what the joint under its loaded
	 * piece must read.
	 *
	 * EVERY ROW ASSERTS THE SAME THING — that the joint reads what ordinary beam theory says
	 * it reads under the force and moment the solver itself reports. That is a stronger claim
	 * than a literal and it survives a fixture being moved: if the arch fires where it must
	 * not, the reading drops to 2|sigma_n|/f_c and the two separate by a factor of hundreds.
	 */
	struct FGateCase
	{
		const TCHAR* Description;

		/** Builds the structure and returns the bed joint whose reading is the subject. */
		TFunction<int32(FStructure&)> Build;

		/** Whether the fixture is supposed to have geometry at all. */
		bool bExpectCompleteGeometry;
	};

	/*
	 * All four fixtures share one shape: a grounded pad, a piece resting on it, and a
	 * neighbour on the eccentric side that is present, intact and Supported. They differ in
	 * exactly one gate each, so a row that starts passing for the wrong reason is visible.
	 */
	constexpr double PadZCm = BrickHeightCm / 2.0;
	constexpr double UpperZCm = PadZCm + CoursePitchCm;

	const TArray<FGateCase> Gates = {
		/*
		 * GATE ONE — NO GEOMETRY AT ALL, and this is the row that keeps 12,000 fuzzed cases
		 * alive. Both fuzz generators emit structures with no joint rectangles and no centres
		 * of mass, and they are the only property tests over routing this project has. A
		 * geometry-free structure must route and read EXACTLY as it does today.
		 *
		 * The shape is the arch's, deliberately: one seat, a live abutting neighbour with a
		 * seat of its own. Everything the rule looks for is there EXCEPT the positions, so an
		 * implementation that reached for the abutment before checking HasCompleteGeometry
		 * would fire here — and would read twice the mean compressive stress instead of once.
		 */
		{
			TEXT("GATE 1: no geometry — the shape of an arch with nobody's position known"),
			[](FStructure& Structure) -> int32
			{
				const int32 Pad = Structure.AddPiece(BrickMassKg, true);
				const int32 Loaded = Structure.AddPiece(BrickMassKg, false);
				const int32 FarPad = Structure.AddPiece(BrickMassKg, true);
				const int32 Neighbour = Structure.AddPiece(BrickMassKg, false);

				FConnection Bed;
				Bed.PieceA = Pad;
				Bed.PieceB = Loaded;
				Bed.InterfaceNormal = FVector::ZAxisVector;
				Bed.InterfaceAreaSqCm = HalfSeatAreaSqCm;
				Bed.Strength = GeneralPurposeMortar;

				const int32 Subject = Structure.AddConnection(Bed);

				FConnection FarBed;
				FarBed.PieceA = FarPad;
				FarBed.PieceB = Neighbour;
				FarBed.InterfaceNormal = FVector::ZAxisVector;
				FarBed.InterfaceAreaSqCm = HalfSeatAreaSqCm;
				FarBed.Strength = GeneralPurposeMortar;
				Structure.AddConnection(FarBed);

				FConnection Head;
				Head.PieceA = Loaded;
				Head.PieceB = Neighbour;
				Head.InterfaceNormal = FVector::XAxisVector;
				Head.InterfaceAreaSqCm = BrickWidthCm * BrickHeightCm;
				Head.Strength = GeneralPurposeMortar;
				Structure.AddConnection(Head);

				return Subject;
			},
			false
		},

		/*
		 * GATE TWO — NO NORMAL FORCE, which is the degenerate end of "the normal force is
		 * compressive". A massless piece on a half seat, geometry and abutment complete, has
		 * sigma_n = 0 AND sigma_b = 0, so k = min(1, 0/0) is a NaN waiting to happen — and a
		 * NaN moment reaches ComputeUtilisation as a bending moment against a real modulus and
		 * comes back as Max(), i.e. a joint that reads as FAILED because nothing was loading
		 * it. The answer has to be exactly zero, finite, and arrived at without the cap
		 * dividing anything by anything.
		 */
		{
			TEXT("GATE 2: a massless piece — no compression, no thrust line, and no 0/0"),
			[](FStructure& Structure) -> int32
			{
				const FPieceBox PadBox = BrickBoxAt(0.0, 0.0, PadZCm);
				const FPieceBox LoadedBox = BrickBoxAt(BondOffsetCm, 0.0, UpperZCm);
				const FPieceBox NeighbourBox = BrickBoxAt(BondOffsetCm + BrickPitchCm, 0.0, UpperZCm);
				const FPieceBox FarPadBox = BrickBoxAt(BrickPitchCm, 0.0, PadZCm);

				const int32 Pad = Structure.AddPiece(BrickMassKg, true, PadBox.CentreCm);
				const int32 Loaded = Structure.AddPiece(0.0, false, LoadedBox.CentreCm);
				const int32 FarPad = Structure.AddPiece(BrickMassKg, true, FarPadBox.CentreCm);
				const int32 Neighbour = Structure.AddPiece(0.0, false, NeighbourBox.CentreCm);

				FConnection Bed;
				MakeInterface(Pad, PadBox, Loaded, LoadedBox, MortarJointCm, GeneralPurposeMortar, Bed);
				const int32 Subject = Structure.AddConnection(Bed);

				FConnection FarBed;
				MakeInterface(
					FarPad, FarPadBox, Neighbour, NeighbourBox,
					MortarJointCm, GeneralPurposeMortar, FarBed);
				Structure.AddConnection(FarBed);

				FConnection Head;
				MakeInterface(
					Loaded, LoadedBox, Neighbour, NeighbourBox,
					MortarJointCm, GeneralPurposeMortar, Head);
				Structure.AddConnection(Head);

				return Subject;
			},
			true
		},

		/*
		 * GATE THREE — ECCENTRIC, BUT INSIDE THE KERN. A brick sitting 1 cm off a pad it
		 * overlaps almost completely loads a 20.5 cm deep patch 0.5 cm off centre, against a
		 * kern that reaches 3.4167 cm. No part of the face is opening, so there is nothing for
		 * an arch to relieve — and min(1, |sigma_n|/sigma_b) is 1 here, which means a correct
		 * implementation is inert by construction. What this row actually catches is the
		 * TEMPTING version without the min: scaling the moment by |sigma_n|/sigma_b
		 * unconditionally would INCREASE this joint's bending stress by a factor of nearly
		 * seven.
		 */
		{
			TEXT("GATE 3: eccentric but INSIDE the kern — nothing is opening"),
			[](FStructure& Structure) -> int32
			{
				const FPieceBox PadBox = BrickBoxAt(0.0, 0.0, PadZCm);
				const FPieceBox LoadedBox = BrickBoxAt(1.0, 0.0, UpperZCm);
				const FPieceBox NeighbourBox = BrickBoxAt(1.0 + BrickPitchCm, 0.0, UpperZCm);
				const FPieceBox FarPadBox = BrickBoxAt(1.0 + BrickPitchCm, 0.0, PadZCm);

				const int32 Pad = Structure.AddPiece(BrickMassKg, true, PadBox.CentreCm);
				const int32 Loaded = Structure.AddPiece(BrickMassKg, false, LoadedBox.CentreCm);
				const int32 FarPad = Structure.AddPiece(BrickMassKg, true, FarPadBox.CentreCm);
				const int32 Neighbour = Structure.AddPiece(BrickMassKg, false, NeighbourBox.CentreCm);

				FConnection Bed;
				MakeInterface(Pad, PadBox, Loaded, LoadedBox, MortarJointCm, GeneralPurposeMortar, Bed);
				const int32 Subject = Structure.AddConnection(Bed);

				FConnection FarBed;
				MakeInterface(
					FarPad, FarPadBox, Neighbour, NeighbourBox,
					MortarJointCm, GeneralPurposeMortar, FarBed);
				Structure.AddConnection(FarBed);

				FConnection Head;
				MakeInterface(
					Loaded, LoadedBox, Neighbour, NeighbourBox,
					MortarJointCm, GeneralPurposeMortar, Head);
				Structure.AddConnection(Head);

				return Subject;
			},
			true
		},

		/*
		 * GATE FOUR — TRAP 3, AND IT IS THE ONE THAT MATTERS. The neighbour on the eccentric
		 * side is intact and reports Supported, so every cheap version of the abutment test
		 * passes — but its ONLY support is the head joint to the piece asking for the arch. Two
		 * bricks propping each other over open air look locally identical to a real arch, and
		 * granting one here would let a wall hang from nothing at all.
		 *
		 * The separating fact is that this neighbour has no bed joint of its own, so the piece
		 * asking IS one of its supports. Read as a load: the half-seated brick carries its own
		 * weight plus the hanger's, and the hanger's weight arrives 11.25 cm out and is carried
		 * a further 11.25 cm across, so the seat sees 33.75 brick-weight-centimetres against
		 * two brick weights of compression — about 0.45 of capacity, comfortably standing, and
		 * a factor of several hundred away from what an arch would read.
		 */
		{
			TEXT("GATE 4: trap 3 — the neighbour is hanging FROM the piece asking for the arch"),
			[](FStructure& Structure) -> int32
			{
				const FPieceBox PadBox = BrickBoxAt(0.0, 0.0, PadZCm);
				const FPieceBox LoadedBox = BrickBoxAt(BondOffsetCm, 0.0, UpperZCm);
				const FPieceBox HangerBox = BrickBoxAt(BondOffsetCm + BrickPitchCm, 0.0, UpperZCm);

				const int32 Pad = Structure.AddPiece(BrickMassKg, true, PadBox.CentreCm);
				const int32 Loaded = Structure.AddPiece(BrickMassKg, false, LoadedBox.CentreCm);
				const int32 Hanger = Structure.AddPiece(BrickMassKg, false, HangerBox.CentreCm);

				FConnection Bed;
				MakeInterface(Pad, PadBox, Loaded, LoadedBox, MortarJointCm, GeneralPurposeMortar, Bed);
				const int32 Subject = Structure.AddConnection(Bed);

				FConnection Head;
				MakeInterface(
					Loaded, LoadedBox, Hanger, HangerBox,
					MortarJointCm, GeneralPurposeMortar, Head);
				Structure.AddConnection(Head);

				return Subject;
			},
			true
		},
	};

	for (const FGateCase& Gate : Gates)
	{
		FStructure Structure;
		const int32 Subject = Gate.Build(Structure);

		if (Subject == INDEX_NONE)
		{
			AddError(FString::Printf(TEXT("%s: FIXTURE: the bed joint was refused"), Gate.Description));
			continue;
		}

		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: HasCompleteGeometry should be %s"),
				Gate.Description, Gate.bExpectCompleteGeometry ? TEXT("true") : TEXT("false")),
			Structure.HasCompleteGeometry() == Gate.bExpectCompleteGeometry);

		Structure.SolveLoads();

		const FConnection& Bed = Structure.GetConnection(Subject);
		const FVector ForceUu = Structure.GetConnectionForce(Subject);
		const FVector MomentUuCm = Structure.GetConnectionMoment(Subject);

		const double Utilisation = Structure.GetConnectionUtilisation(Subject);

		const FBedJointReading Expected = ReadBedJoint(
			ForceUu, MomentUuCm, Bed.InterfaceHalfExtentCm, Bed.InterfaceAreaSqCm,
			GeneralPurposeMortar);

		const double Arched = ArchedUtilisation(Expected, GeneralPurposeMortar);

		AddInfo(FString::Printf(
			TEXT("%s: joint carries %s uu and %s uu.cm; reads %s, beam theory says %s, an arch ")
			TEXT("would read %s"),
			Gate.Description, *Bits(ForceUu.Size()), *Bits(MomentUuCm.Size()),
			*Bits(Utilisation), *Bits(Expected.Worst), *Bits(Arched)));

		/*
		 * NEVER NaN AND ALWAYS FINITE, FIRST, because every comparison against a NaN is false
		 * and an assertion written the obvious way would PASS for one. FMath::Max discards a
		 * NaN and FMath::Min replaces it, so a degenerate input becomes a plausible number
		 * rather than an obvious fault unless somebody looks for it on purpose.
		 */
		TestTrue(
			FString::Printf(TEXT("%s: the utilisation must be finite, it is %s"),
				Gate.Description, *Bits(Utilisation)),
			FMath::IsFinite(Utilisation));

		TestTrue(
			FString::Printf(
				TEXT("%s: the arch must NOT fire — beam theory on the solver's own force and ")
				TEXT("moment says %s, the joint reads %s"),
				Gate.Description, *Bits(Expected.Worst), *Bits(Utilisation)),
			FMath::Abs(Utilisation - Expected.Worst) <= 1.0e-12 * FMath::Max(Expected.Worst, 1.0e-12));
	}

	/*
	 * ===================================================================================
	 * PART 4 — THE REGRESSION ANCHORS THIS SLICE IS MOST LIKELY TO DESTROY.
	 * ===================================================================================
	 */

	/*
	 * ANCHOR 2 — THE STAIRCASE CORBEL, AND IT IS THE DIRECTION CHECK.
	 *
	 * The corbel's eccentric side is the side the cut REMOVED the neighbour from, so there is
	 * no head joint there at all and the arch must be refused. Apply the cap without checking
	 * which side the abutment is on and this number collapses from 22.93 to about 0.005, the
	 * photographed failure stands up, and the whole reason this subsystem's test suite exists
	 * evaporates. The staircase's own test would fail too — the suite contains its own guard —
	 * and re-asserting it here is what makes that guard say the word "arch" when it fires.
	 *
	 * The tolerance is the staircase test's own 1e-5 relative, for the reason recorded there:
	 * the hand ladder's load cone reaches past the end of a 10-brick-wide wall, which is worth
	 * 1.0e-7 on the bottom rung.
	 */
	{
		FBrickLayout Staircase;

		if (RunningBond(StaircaseWallSpec(), Staircase)
			&& Staircase.Boxes.Num() == StaircaseWallPieceCount)
		{
			for (const int32 Piece : StaircaseVoidPieces(Staircase.Boxes))
			{
				Staircase.Structure.RemovePiece(Piece);
			}

			Staircase.Structure.SolveLoads();

			int32 OverCapacity = 0;
			double Worst = 0.0;

			for (int32 Course = StaircaseLowestCorbelCourse;
				Course <= StaircaseHighestCorbelCourse; ++Course)
			{
				const int32 Corbel = StaircaseCorbelPiece(Staircase.Boxes, Course);
				const int32 Support = StaircaseCorbelSupportPiece(Staircase.Boxes, Course);
				const int32 Joint = JointBetweenPieces(Staircase.Structure, Corbel, Support);

				if (Joint == INDEX_NONE)
				{
					AddError(FString::Printf(
						TEXT("ANCHOR 2: FIXTURE: course %d has no corbel joint"), Course));

					continue;
				}

				const double Utilisation = Staircase.Structure.GetConnectionUtilisation(Joint);

				Worst = FMath::Max(Worst, Utilisation);

				if (Utilisation > 1.0)
				{
					++OverCapacity;
				}
			}

			AddInfo(FString::Printf(
				TEXT("ANCHOR 2: the staircase corbel reads %s at its bottom rung with %d of %d ")
				TEXT("over capacity (the hand ladder says %.8f and %d)"),
				*Bits(Worst), OverCapacity, StaircaseCorbelStepCount,
				StaircasePredictedWorstCorbelUtilisation,
				StaircasePredictedCorbelJointsOverCapacity));

			TestTrue(
				FString::Printf(
					TEXT("ANCHOR 2: the corbel's eccentric side has NO neighbour, so it must NOT ")
					TEXT("arch — it must still read %.8f, it reads %s"),
					StaircasePredictedWorstCorbelUtilisation, *Bits(Worst)),
				FMath::Abs(Worst - StaircasePredictedWorstCorbelUtilisation)
					<= 1.0e-5 * StaircasePredictedWorstCorbelUtilisation);

			TestEqual(
				TEXT("ANCHOR 2: and eight of its eleven rungs must still be over capacity"),
				OverCapacity, StaircasePredictedCorbelJointsOverCapacity);
		}
		else
		{
			AddError(TEXT("ANCHOR 2: FIXTURE: the staircase wall did not lay"));
		}
	}

	/*
	 * ANCHOR 3 — THE NARROW-WAIST WALL, AND IT IS THE DIRECTION CHECK FOR THE ABUTMENT RULE.
	 *
	 *      course 2         [ 3 ][ 4 ]
	 *      course 1            [ 2 ]        THE WAIST
	 *      course 0         [ 0 ][ 1 ]      grounded
	 *
	 * Bricks 3 and 4 each rest on the waist and on nothing else — N = 1, the determinate case —
	 * and each one overhangs it OUTWARD, away from the other. Their shared head joint sits
	 * between them, which is on brick 3's SEATED side and on brick 4's SEATED side. So a live,
	 * intact, Supported neighbour exists for both and neither may have it: the abutment has to
	 * be on the ECCENTRIC side or it is not an abutment, it is the wall.
	 *
	 * This is the cheapest possible fixture for that distinction and it is why the figure is
	 * re-asserted here rather than left to StructureBinding.AdoptedWallLoadsItsWaistEccentrically:
	 * an implementation that merely asked "is there an intact head joint?" would pass every
	 * other anchor in this file and quietly halve every overhang in the game.
	 */
	{
		FRunningBondSpec WaistSpec;
		WaistSpec.BrickSizeCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm);
		WaistSpec.JointThicknessCm = MortarJointCm;
		WaistSpec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		WaistSpec.CoursesHigh = 3;
		WaistSpec.BricksPerCourse = 2;
		WaistSpec.End = EWallEnd::Ragged;
		WaistSpec.Strength = GeneralPurposeMortar;

		FBrickLayout Waist;

		if (RunningBond(WaistSpec, Waist) && Waist.Structure.NumPieces() == 5)
		{
			Waist.Structure.SolveLoads();

			constexpr int32 WaistedBrick = 3;

			int32 Subject = INDEX_NONE;

			for (int32 Joint = 0; Joint < Waist.Structure.NumConnections(); ++Joint)
			{
				if (Waist.Structure.GetJointRole(Joint, WaistedBrick) == EJointRole::BedBeneath)
				{
					Subject = Joint;
				}
			}

			if (Subject != INDEX_NONE)
			{
				const double Utilisation = Waist.Structure.GetConnectionUtilisation(Subject);

				AddInfo(FString::Printf(
					TEXT("ANCHOR 3: the waisted brick's bed joint reads %s"), *Bits(Utilisation)));

				/*
				 * 2667.198625 * 5.625 / 179.4817708 - 2667.198625 / 105.0625, over 10000, over
				 * f_xk1 — MOMENTS_DESIGN's case (c), and 0.058203838 is what it comes to.
				 */
				constexpr double WaistUtilisation = 0.058203838;

				TestTrue(
					FString::Printf(
						TEXT("ANCHOR 3: the waisted brick's head joint is on its SEATED side, so ")
						TEXT("it must NOT arch — it must still read %g, it reads %s"),
						WaistUtilisation, *Bits(Utilisation)),
					FMath::Abs(Utilisation - WaistUtilisation) <= 1.0e-8);
			}
			else
			{
				AddError(TEXT("ANCHOR 3: FIXTURE: the waisted brick has no bed joint beneath it"));
			}
		}
		else
		{
			AddError(TEXT("ANCHOR 3: FIXTURE: the narrow-waist wall did not lay"));
		}
	}

	/*
	 * ANCHOR 4 — A HEAD JOINT IS NOT A SPRINGING, AND THE CAP IS BedBeneath-ONLY.
	 *
	 * MOMENTS_DESIGN's case (b): a brick held by one head joint, whose mean normal stress is
	 * EXACTLY zero because gravity runs parallel to the joint. That is the one visible thing
	 * the moment work bought — the joint reads 0.4157 in tension instead of 0.0200 in shear,
	 * and a chain of two reads 0.8315 and still holds.
	 *
	 * IT IS ALSO THE SHARPEST TEST OF THE CAP'S SHAPE THERE IS. With sigma_n exactly zero,
	 * k = min(1, 0/sigma_b) is exactly ZERO, so applying the cap to a head joint does not
	 * merely relieve this joint, it DELETES it — straight back to the 0.0200 shear reading the
	 * moment work was written to replace. Nothing else in this file has a zero normal stress,
	 * so nothing else can catch that.
	 */
	{
		struct FChainCase
		{
			const TCHAR* Description;
			int32 BricksInChain;
			double Expected;
		};

		const TArray<FChainCase> Chains = {
			{ TEXT("ANCHOR 4: one brick on one head joint"), 1, 0.4157273077 },
			{ TEXT("ANCHOR 4: two bricks on the same head joint"), 2, 0.8314546154 },
		};

		for (const FChainCase& Chain : Chains)
		{
			FStructure Structure;

			const FPieceBox PadBox = BrickBoxAt(0.0, 0.0, PadZCm);
			const int32 Pad = Structure.AddPiece(BrickMassKg, true, PadBox.CentreCm);

			TArray<FPieceBox> Boxes;
			TArray<int32> Handles;

			for (int32 Brick = 0; Brick < Chain.BricksInChain; ++Brick)
			{
				const FPieceBox Box =
					BrickBoxAt(BrickPitchCm, 0.0, PadZCm + Brick * CoursePitchCm);

				Boxes.Add(Box);
				Handles.Add(Structure.AddPiece(BrickMassKg, false, Box.CentreCm));
			}

			FConnection Head;
			MakeInterface(
				Pad, PadBox, Handles[0], Boxes[0], MortarJointCm, GeneralPurposeMortar, Head);
			const int32 Subject = Structure.AddConnection(Head);

			for (int32 Brick = 1; Brick < Chain.BricksInChain; ++Brick)
			{
				FConnection Bed;
				MakeInterface(
					Handles[Brick - 1], Boxes[Brick - 1],
					Handles[Brick], Boxes[Brick],
					MortarJointCm, GeneralPurposeMortar, Bed);
				Structure.AddConnection(Bed);
			}

			Structure.SolveLoads();

			const double Utilisation = Structure.GetConnectionUtilisation(Subject);

			AddInfo(FString::Printf(TEXT("%s: reads %s, expected %.10f"),
				Chain.Description, *Bits(Utilisation), Chain.Expected));

			TestTrue(
				FString::Printf(
					TEXT("%s: a head joint is not a springing, so it must still read %.10f; it ")
					TEXT("reads %s"),
					Chain.Description, Chain.Expected, *Bits(Utilisation)),
				FMath::Abs(Utilisation - Chain.Expected) <= 1.0e-9 * Chain.Expected);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
