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
namespace StructureOneCellThrustTestSupport
{
	using namespace DestructionProfiles;
	using namespace StructureArchingTestSupport;

	/**
	 * ONE ROW OF THE TABLE: the same wall, the same cut, the same one-cell arch — and a joint
	 * that either can or cannot deliver the sideways push that arch is made of.
	 *
	 * THE PROFILE IS THE ONLY THING THAT MOVES. Geometry decides how hard the arch pushes and
	 * the profile decides what the springing can take, so varying the profile alone is what
	 * makes this a statement about CAPACITY rather than about shape.
	 */
	struct FThrustCase
	{
		const TCHAR* Description;

		FConnectionStrength Strength;

		/** Whether the springing can carry the horizontal push the moment cap assumes. */
		bool bCanCarryTheThrust;

		/**
		 * What fraction of its sliding capacity that push comes to, worked by hand at the 28
		 * brick weights this cut leaves on the seat. Held per row rather than derived, so the
		 * fixture and the arithmetic have to agree rather than agreeing with themselves.
		 */
		double DemandOverCapacityAt28BrickWeights;
	};

	/**
	 * HOW HARD A JOINT MAY BE PUSHED SIDEWAYS, MPa — Mohr-Coulomb, written out here rather than
	 * imported, because a test that reached for production's own expression would agree with a
	 * wrong one. `c + mu * sigma`, truncated at the profile's own ceiling.
	 *
	 * The compression is taken as a magnitude: `ReadBedJoint` publishes sigma_n signed and
	 * positive in tension, and only a squeeze buys friction.
	 */
	inline double SlidingCapacityMPa(const FConnectionStrength& Strength, double NormalStressMPa)
	{
		return FMath::Min(
			Strength.ShearCohesionMPa + Strength.FrictionCoefficient * FMath::Abs(NormalStressMPa),
			Strength.MaxShearStrengthMPa);
	}
}

/**
 * THE ONE-CELL ARCHING RELIEF HAS TO BE EARNED: THE SPRINGING MUST BE ABLE TO CARRY THE SIDEWAYS
 * PUSH THE MOMENT CAP ASSUMES, AND A JOINT THAT CANNOT MUST NOT BE GRANTED IT.
 *
 * ============================================================================================
 * WHAT THE CAP ACTUALLY ASSUMES — DERIVED FROM THE CAP ITSELF, NOT FROM BS 5977
 * ============================================================================================
 *
 * Delete one brick from a running-bond wall and the brick above keeps exactly one 10.25 x 10.25
 * seat, with its load arriving e = 5.625 cm outside that patch's centroid. `ArchingMomentScale`
 * then caps the whole moment vector by k = |sigma_n| / sigma_b, which is the same statement as
 *
 *     M' = k * M = (N/A) * W = N * h/6        h/6 = 10.25/6 = 1.7083333 cm
 *
 * — the thrust line is moved from 5.625 cm out to the kern edge at 1.7083 cm out. That is what an
 * arch IS, and DESIGN §5.4 is explicit that it is correct physics WHEN THE ABUTMENTS CAN TAKE THE
 * THRUST. What it is not is free. Take moments about the seat's own centroid on the free body of
 * the half-seated brick:
 *
 *     F * e     (overturning, from the column the solver accumulated)
 *   = M'        (what the joint is left carrying)
 *   + H * z     (the couple something else has to supply)
 *
 * so the couple the cap DELETES is
 *
 *     dM = F * (e - h/6) = F * 3.9166667 cm
 *
 * and nothing on that free body can supply it except a horizontal pair: a push H from the abutment
 * through the intact head joint gate four insists on, and its equal and opposite reaction as SHEAR
 * in the seat's own bed plane. The arm between the two is measured, not assumed — it is the head
 * joint's own centroid above the bed joint's own centroid, which for this brick and this 1 cm
 * mortar joint is 3.75 cm — so
 *
 *     H / V = (e - h/6) / z = 3.9166667 / 3.75 = 1.0444444
 *
 * with F CANCELLED OUT. It is a statement about the bond geometry alone, exactly as the spanned
 * case's H/V = 3L/(4*d_e) is: the same push at every wall height and every load.
 *
 * TODAY THAT PUSH IS NEITHER APPLIED NOR CHECKED. The thrust pass (`ApplyArchingThrust`) only ever
 * runs over arches recorded by `ReseatSpannedGroups`, and a one-cell hole leaves nobody seatless,
 * so no group forms, no arch is recorded, and the springing's force vector stays (0, 0, -F) exactly
 * — DESIGN §7 gap 4, and `StructureThrustTest`'s own header names this as the case its rows cannot
 * see. The relief is granted fail-open, on topology alone.
 *
 * HOW SENSITIVE THE VERDICTS BELOW ARE TO THAT ARM, STATED RATHER THAN LEFT TO BE DISCOVERED. The
 * only free choice in the derivation is where inside the head joint's face the push acts:
 *
 *   - at the head joint's CENTROID, 3.75 cm up:                H/V = 1.0444
 *   - at the head joint's own KERN EDGE, 3.75 + 6.5/6 = 4.8333: H/V = 0.8103
 *   - by the model's spanned formula read at one cell,
 *     L = 11.25 cm under 202.5 cm of cover, so the angle
 *     governs and H/V = 3/(4*0.866):                            H/V = 0.8660
 *
 * Every row in the table below reads the same verdict under all three. The arm that would flip the
 * dry-stone row is 3.9166667/0.7 = 5.5952 cm, which puts the thrust line 1.85 cm past the head
 * joint's CENTROID and 0.76 cm past its kern edge (4.8333 cm) — an arch delivered through a joint
 * it is simultaneously prising open. The assertions are written against the measured 3.75 cm arm
 * and the margins are stated with each row.
 *
 * ============================================================================================
 * THE TABLE, AND THE ARITHMETIC BEHIND EACH ROW
 * ============================================================================================
 *
 * One wall — the 7 x 30 flush wall `StructureArchingTest` anchors at 0.0141885 — with one interior
 * brick cut out of course 1, laid three times in three profiles. At 28 brick weights the surviving
 * seat carries sigma_n = 74681.56 / (105.0625 * 10000) = 0.0710835 MPa, so the push it has to
 * deliver is 1.0444444 * 0.0710835 = 0.0742428 MPa, and:
 *
 *   GENERAL PURPOSE MORTAR — c = 0.2, mu = 0.6.  Capacity 0.2 + 0.6*0.0710835 = 0.2426501 MPa,
 *     so the demand is 0.3060 of it. THE ARCH IS EARNED and this wall must go on standing. Note
 *     the margin is NOT load-independent — cohesion is a constant while the demand is linear in
 *     the load — and mortar only runs out at sigma_n = 0.45 MPa, which is 177 brick weights on one
 *     half seat, roughly a 180-course wall. Nothing in this project is within a factor of four
 *     of it.
 *
 *   LIME MORTAR — c = 0.1, mu = 0.6.  Capacity 0.1426501 MPa, so the demand is 0.5205 of it. The
 *     arch is still earned, and this row is the one that makes the pair a GATE rather than a
 *     one-way ratchet: measured, withholding the relief everywhere leaves general purpose mortar
 *     standing at 0.646 — the composite deep beam of §5.5 catching it, not the arch — while lime's
 *     weaker bond reads about 1.29 in tension and the wall comes down. So an implementation that
 *     closes gap 4 by refusing the relief wholesale fails HERE, and only here.
 *
 *   DRY STONE — c = 0.0, mu = 0.7.  Zero cohesion, so sigma_n cancels twice over and the reading
 *     is 1.0444444/0.7 = 1.4920635 AT EVERY LOAD AND EVERY WALL HEIGHT. THE ARCH IS NOT EARNED,
 *     and this is the same sentence DESIGN §5.4 already writes for the spanned case (0.866/0.7 =
 *     1.2372 at every span): you cannot span a hole in a dry-stone wall with a flat arch. The
 *     one-cell hole is the case that sentence does not currently reach.
 *
 * WHY THERE IS NO ZERO-COHESION ROW WITH A TENSILE BOND, which is the row this table wanted and
 * cannot have. `CohesionlessBond` — dry stone's friction with a real 0.08 MPa tensile bond — was
 * written and measured, and it entangles two gaps rather than isolating this one: withhold the
 * unearned relief and the joint does NOT fail, because composite vertical action re-sections its
 * moment and it reads 0.807. That is DESIGN §7 gap 5 (the deep beam's horizontal shear flow is
 * never applied as a demand either) standing directly behind gap 4, and a row that can only be
 * satisfied by closing both is a row that sends the wrong instruction. Recorded here rather than
 * left to be rediscovered.
 *
 * ============================================================================================
 * WHAT IS ASSERTED, AND WHY IN THAT FORM
 * ============================================================================================
 *
 *   - THE MECHANISM, on the named seat: an unearned springing must read OVER CAPACITY and an
 *     earned one must read under it. Today the unearned rows read 2*0.0710835/30 = 0.0047389,
 *     which is the arched compression answer and is the whole defect.
 *
 *   - THE OUTCOME, as a count of joints that FAILED UNDER LOAD after a cascade, and as where the
 *     brick ends up. A single severed joint is not a collapse and a wall can sever one and still
 *     stand, so the earned rows' claim is that NOTHING failed under load, and the unearned row's is
 *     that both unearned springings did — with a break pass of their own — and that the two bricks
 *     they were holding up end FALLING rather than stranded or standing. Counted by break pass and
 *     never by `HasGiven`: the player's own deletion always takes six joints with it and those
 *     never snapped.
 *
 *   - NEVER A DISPLACEMENT. Nothing here moves; two pieces can sever their bond and stay resting
 *     exactly where they were, so how far anything travelled would say nothing about the mortar.
 *
 *   - AND NEVER WHICH MECHANISM CLOSES THE GATE. Applying the thrust to the springing as a real
 *     shear demand and withholding the relief where the springing cannot carry it are both answers
 *     to this test: under the first the unearned seat reads 1.4920635 in shear, under the second it
 *     reads its un-arched tension, which for dry stone's exact zero is Max. Both are over capacity,
 *     and both leave the two earned rows standing — the first at 0.3055 and 0.5205 in shear, the
 *     second untouched at 0.0142 and 0.0709. The one thing neither may do is what happens today,
 *     which is nothing at all.
 *
 * WHICH AXIS GOVERNS, WORKED FOR EVERY ROW BEFORE ANYTHING IS CLAIMED. `ComputeUtilisation` returns
 * the worst of compression, shear and tension, so a fixture aimed at sliding silently measures
 * something else the moment something else is higher. On the earned rows compression governs today
 * at 2|sigma_n|/f_c — 0.0142 in cement and 0.0709 in lime (tension is exactly zero at the cap, and
 * shear is exactly zero because no thrust is applied anywhere); on the unearned row compression
 * governs today at 0.0047, and after the gate closes the governing axis becomes whichever of shear
 * or tension the implementation puts the demand on. The claim is therefore written as "over
 * capacity" rather than as a figure on a nominated axis, and every axis is printed.
 *
 * NEEDS A TICKING WORLD: NO. `FStructure` is plain arithmetic over a graph and `Layout` is plain
 * arithmetic over boxes; the 980 is the solver's own constant, not a physics scene's.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureOneCellThrustTest,
	"DestructionGame.Core.Structure.AOneCellArchMustEarnItsThrust",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureOneCellThrustTest::RunTest(const FString& Parameters)
{
	using namespace StructureOneCellThrustTestSupport;
	using namespace StaircaseWallTestSupport;

	/*
	 * THE EXPECTED NUMBERS ARE RATIOS OF PUBLISHED STRENGTHS, so they mean what they say only
	 * while the profiles still carry the figures they were derived against. Asserted rather than
	 * imported: a test that read the profile would agree with a wrong profile.
	 */
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mortar cohesion 0.2 MPa, the profile carries %g"),
			GeneralPurposeMortar.ShearCohesionMPa),
		GeneralPurposeMortar.ShearCohesionMPa == 0.2);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mortar friction 0.6, the profile carries %g"),
			GeneralPurposeMortar.FrictionCoefficient),
		GeneralPurposeMortar.FrictionCoefficient == 0.6);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mortar compressive 10 MPa, the profile carries %g"),
			GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == 10.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: dry stone's cohesion must be an EXACT zero, it is %g"),
			DryStone.ShearCohesionMPa),
		DryStone.ShearCohesionMPa == 0.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against dry stone friction 0.7, the profile carries %g"),
			DryStone.FrictionCoefficient),
		DryStone.FrictionCoefficient == 0.7);

	TestTrue(
		FString::Printf(
			TEXT("FIXTURE: derived against lime cohesion 0.1 MPa, tensile 0.05 MPa and compressive ")
			TEXT("2.0 MPa; the profile carries %g, %g and %g"),
			LimeMortar.ShearCohesionMPa, LimeMortar.TensileStrengthMPa,
			LimeMortar.CompressiveStrengthMPa),
		LimeMortar.ShearCohesionMPa == 0.1 && LimeMortar.TensileStrengthMPa == 0.05
			&& LimeMortar.CompressiveStrengthMPa == 2.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == 1.9);

	/**
	 * THE ARM THE CAP'S COUPLE ACTS OVER, as a fixture expectation rather than as an input: it is
	 * MEASURED off each joint below and compared against this. Half a brick plus half a mortar
	 * joint is the rise from a bed plane to the head joint centroid in the course above it.
	 */
	constexpr double ExpectedThrustArmCm = BrickHeightCm / 2.0 + MortarJointCm / 2.0;

	/** (5.625 - 1.7083333) / 3.75, and the load has cancelled out of it. */
	constexpr double ExpectedThrustPerReaction = 1.0444444444444445;

	const TArray<FThrustCase> Cases = {
		/*
		 * THE ANCHOR'S OWN WALL. A mortared springing is asked for 0.0742 MPa of sliding against
		 * 0.2427 MPa of bond plus friction — 0.306 of capacity, a margin of 3.27 — so the relief
		 * is correctly granted and this wall must go on standing exactly as it does today. This is
		 * the fixture `StructureArchingTest` pins at 0.0141885, re-entered here so that a gate
		 * which closed on everything fails in the file that closed it.
		 */
		{ TEXT("EARNED: general purpose mortar, c = 0.2 + 0.6 sigma"), GeneralPurposeMortar,
			true, 0.30596 },

		/*
		 * LIME, AND IT IS THE ROW THAT MAKES THIS PAIR A GATE RATHER THAN A ONE-WAY RATCHET.
		 *
		 * Half the cohesion, so the push costs 0.0742 MPa against 0.1427 MPa — 0.520 of capacity,
		 * still comfortably affordable, so the relief is still earned and this wall must stand.
		 * What separates it from the row above is what happens if an implementation closes the gate
		 * by withholding the relief EVERYWHERE: measured, general purpose mortar survives that at
		 * 0.646 (the composite deep beam catches it, not the arch) while lime's weaker bond reads
		 * about 1.29 in tension and the wall comes down. So this row is the one that fails if the
		 * relief stops being granted where it is earned — the property the whole slice must not
		 * break — and it was proven to bite by exactly that mutation.
		 */
		{ TEXT("EARNED: lime mortar, c = 0.1 + 0.6 sigma"), LimeMortar, true, 0.52045 },

		/*
		 * DRY STONE, WHERE THE PUSH IS THE ONLY THING THAT CAN FAIL. Cohesion is an exact zero, so
		 * capacity is 0.7*sigma against a demand of 1.0444*sigma and sigma cancels: 1.4921 of
		 * capacity at every load, every wall height and every brick density — the same shape as
		 * the spanned case's 0.866/0.7 = 1.2372, which DESIGN §5.4 already states outright and
		 * which the one-cell hole is the case it does not reach.
		 */
		{ TEXT("UNEARNED: dry stone, c = 0, mu = 0.7"), DryStone, false, 1.4920634920634921 },
	};

	for (const FThrustCase& Case : Cases)
	{
		/*
		 * THE SAME WALL AND THE SAME CUT AS THE ARCHING ANCHOR — 7 x 30, flush, the third full
		 * brick of course 1 — with the profile as the only difference. A second wall definition is
		 * two fixtures that drift, so the spec comes from the shared header and one field is
		 * overwritten.
		 */
		FRunningBondSpec Spec = ArchWallSpec();
		Spec.Strength = Case.Strength;

		FBrickLayout Intact;

		if (!RunningBond(Spec, Intact) || Intact.Boxes.Num() != ArchWallPieceCount)
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE: a flush 7 x 30 wall should lay as %d pieces, got %d"),
				Case.Description, ArchWallPieceCount, Intact.Boxes.Num()));

			continue;
		}

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the laid wall must know where every piece and every joint is, or ")
				TEXT("every moment below is silently zero and this measures nothing"),
				Case.Description),
			Intact.Structure.HasCompleteGeometry());

		/*
		 * THE WALL HAS TO STAND BEFORE IT IS CUT, or a row about one deletion is measuring the
		 * material instead. Every seat of an intact running bond has e = 0 exactly, so nothing in
		 * here carries a moment whatever the profile is.
		 */
		Intact.Structure.SolveLoads();

		double IntactWorst = 0.0;

		for (int32 Joint = 0; Joint < Intact.Structure.NumConnections(); ++Joint)
		{
			IntactWorst = FMath::Max(IntactWorst, Intact.Structure.GetConnectionUtilisation(Joint));
		}

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the INTACT wall must be nowhere near capacity, its worst joint ")
				TEXT("reads %s"),
				Case.Description, *Bits(IntactWorst)),
			IntactWorst < 0.1);

		FBrickLayout Cut;

		if (!RunningBond(Spec, Cut) || Cut.Boxes.Num() != ArchWallPieceCount)
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE: the second copy of the wall did not lay"), Case.Description));

			continue;
		}

		const int32 DeletedPiece = StaircasePieceAt(
			Cut.Boxes, DeletedBrickXCm, ArchWallCourseZCm(DeletedBrickCourse));

		if (DeletedPiece == INDEX_NONE || !Cut.Structure.RemovePiece(DeletedPiece))
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE: there should be a brick at x %.2f in course %d to delete"),
				Case.Description, DeletedBrickXCm, DeletedBrickCourse));

			continue;
		}

		Cut.Structure.SolveLoads();

		/**
		 * The two bricks the deletion left standing on half a seat, each overhanging TOWARD the
		 * other — so the head joint between them is on each one's eccentric side, and each is the
		 * other's abutment. That is the one-cell arch.
		 */
		struct FSpringing
		{
			const TCHAR* Description;
			double BrickXCm;
			double AbutmentXCm;
		};

		const TArray<FSpringing> Springings = {
			{ TEXT("left springing"), LeftHalfSeatedXCm, RightHalfSeatedXCm },
			{ TEXT("right springing"), RightHalfSeatedXCm, LeftHalfSeatedXCm },
		};

		TArray<int32> SpringingSeatJoints;
		TArray<int32> SpringingBricks;

		for (const FSpringing& Springing : Springings)
		{
			const int32 Brick = StaircasePieceAt(
				Cut.Boxes, Springing.BrickXCm, ArchWallCourseZCm(HalfSeatedCourse));
			const int32 Abutment = StaircasePieceAt(
				Cut.Boxes, Springing.AbutmentXCm, ArchWallCourseZCm(HalfSeatedCourse));

			const int32 SeatJoint = Brick == INDEX_NONE
				? INDEX_NONE
				: TheOneIntactSeatBeneath(Cut.Structure, Brick);

			const int32 HeadJoint = Brick == INDEX_NONE || Abutment == INDEX_NONE
				? INDEX_NONE
				: JointBetweenPieces(Cut.Structure, Brick, Abutment);

			if (SeatJoint == INDEX_NONE || HeadJoint == INDEX_NONE)
			{
				AddError(FString::Printf(
					TEXT("%s, %s: FIXTURE: it must keep EXACTLY ONE seat (joint %d) and one head ")
					TEXT("joint to its abutment (joint %d)"),
					Case.Description, Springing.Description, SeatJoint, HeadJoint));

				continue;
			}

			const FConnection& Seat = Cut.Structure.GetConnection(SeatJoint);
			const FConnection& Head = Cut.Structure.GetConnection(HeadJoint);

			/*
			 * GATE FOUR, AS A FIXTURE PRECONDITION: the abutment reaches the ground on its own
			 * account. Without that there is no arch to earn, and the row below would be measuring
			 * a refused cap rather than an unearned one.
			 */
			TestTrue(
				FString::Printf(
					TEXT("%s, %s: FIXTURE: the abutment must be Supported or Grounded on its own ")
					TEXT("account, and the half-seated brick must still be Supported"),
					Case.Description, Springing.Description),
				(Cut.Structure.GetPieceSupport(Abutment) == EPieceSupport::Supported
					|| Cut.Structure.GetPieceSupport(Abutment) == EPieceSupport::Grounded)
					&& Cut.Structure.GetPieceSupport(Brick) == EPieceSupport::Supported);

			/*
			 * THE GEOMETRY THE IMPLIED THRUST IS BUILT FROM, ALL THREE LENGTHS MEASURED OFF THE
			 * FIXTURE RATHER THAN ASSUMED — the overhang, the seat's own kern, and the arm from the
			 * bed plane up to the head joint the push has to arrive through.
			 */
			const double EccentricityCm = FMath::Abs(
				Cut.Boxes[Brick].CentreCm.X - Seat.InterfaceCentreCm.X);

			const double KernCm = KernFromHalfExtentCm(Seat.InterfaceHalfExtentCm.X);

			const double ThrustArmCm = Head.InterfaceCentreCm.Z - Seat.InterfaceCentreCm.Z;

			TestTrue(
				FString::Printf(
					TEXT("%s, %s: FIXTURE: it must overhang its seat by %g cm against a kern of %g cm ")
					TEXT("(it reads %s and %s), with the head joint %g cm above the bed plane (it ")
					TEXT("reads %s)"),
					Case.Description, Springing.Description, HalfSeatEccentricityCm,
					BrickWidthCm / 6.0, *Bits(EccentricityCm), *Bits(KernCm),
					ExpectedThrustArmCm, *Bits(ThrustArmCm)),
				FMath::IsNearlyEqual(EccentricityCm, HalfSeatEccentricityCm, 1.0e-9)
					&& FMath::IsNearlyEqual(KernCm, BrickWidthCm / 6.0, 1.0e-9)
					&& FMath::IsNearlyEqual(ThrustArmCm, ExpectedThrustArmCm, 1.0e-9));

			/*
			 * AND THE PUSH ITSELF, WITH THE LOAD CANCELLED OUT OF IT. dM = F*(e - h/6) is the couple
			 * the cap deletes and H*z is the only thing on this free body that can supply it, so
			 * H/V is a fact about the bond and nothing else — the same shape as the spanned case's
			 * 3L/(4*d_e).
			 */
			const double ImpliedThrustPerReaction = (EccentricityCm - KernCm) / ThrustArmCm;

			TestTrue(
				FString::Printf(
					TEXT("%s, %s: FIXTURE: the cap implies H/V = (%g - %g)/%g = %s, and the fixture ")
					TEXT("measures %s"),
					Case.Description, Springing.Description, HalfSeatEccentricityCm,
					BrickWidthCm / 6.0, ExpectedThrustArmCm, *Bits(ExpectedThrustPerReaction),
					*Bits(ImpliedThrustPerReaction)),
				FMath::Abs(ImpliedThrustPerReaction - ExpectedThrustPerReaction)
					<= 1.0e-12 * ExpectedThrustPerReaction);

			const FVector ForceUu = Cut.Structure.GetConnectionForce(SeatJoint);
			const FVector MomentUuCm = Cut.Structure.GetConnectionMoment(SeatJoint);

			const double Utilisation = Cut.Structure.GetConnectionUtilisation(SeatJoint);

			const FBedJointReading Published = ReadBedJoint(
				ForceUu, MomentUuCm, Seat.InterfaceHalfExtentCm, Seat.InterfaceAreaSqCm,
				Case.Strength);

			/*
			 * WHAT THE SPRINGING IS ASKED FOR AGAINST WHAT IT CAN GIVE, both in MPa and both built
			 * here from the solver's own reported force. Only the FORCE comes from production; the
			 * conversion, the stress, the Mohr-Coulomb capacity and the ratio are all this file's.
			 */
			const double DemandMPa =
				ImpliedThrustPerReaction * FMath::Abs(Published.NormalStressMPa);

			const double CapacityMPa = SlidingCapacityMPa(Case.Strength, Published.NormalStressMPa);

			const double DemandOverCapacity = DemandMPa / CapacityMPa;

			AddInfo(FString::Printf(
				TEXT("%s, %s: joint %d carries (%s, %s, %s) uu = %.4f brick weights; sigma_n %s MPa; ")
				TEXT("the cap implies a push of %s MPa against a sliding capacity of %s MPa — %s of ")
				TEXT("it; the joint reads %s (tension %s, compression %s, shear %s)"),
				Case.Description, Springing.Description, SeatJoint,
				*Bits(ForceUu.X), *Bits(ForceUu.Y), *Bits(ForceUu.Z),
				ForceUu.Size() / BrickWeightUu, *Bits(Published.NormalStressMPa),
				*Bits(DemandMPa), *Bits(CapacityMPa), *Bits(DemandOverCapacity),
				*Bits(Utilisation), *Bits(Published.TensionUtilisation),
				*Bits(Published.CompressionUtilisation), *Bits(Published.ShearUtilisation)));

			/*
			 * AND WHAT THE HEAD JOINT THE CAP LEANS ON IS CARRYING, WHICH IS THE DEFECT IN ONE
			 * NUMBER. Reported rather than asserted: whether the thrust ends up travelling through
			 * this joint or is only checked against the springing's capacity is the implementation's
			 * choice, and this test does not make it.
			 */
			AddInfo(FString::Printf(
				TEXT("%s, %s: the head joint %d the relief leans on carries %s uu"),
				Case.Description, Springing.Description, HeadJoint,
				*Bits(Cut.Structure.GetConnectionForce(HeadJoint).Size())));

			TestTrue(
				FString::Printf(
					TEXT("%s, %s: FIXTURE: the seat must be in COMPRESSION or there is no thrust ")
					TEXT("line and no friction to borrow; sigma_n is %s MPa"),
					Case.Description, Springing.Description, *Bits(Published.NormalStressMPa)),
				Published.NormalStressMPa < 0.0);

			/*
			 * THE ARITHMETIC OF THE ROW, ASSERTED BEFORE ITS VERDICT IS, TO TWO PERCENT OF THE
			 * HAND FIGURE — the slack is for the wall's real load distribution being 27.94 brick
			 * weights rather than exactly 28, and for nothing else. A wrong lever arm, a wrong
			 * kern, a missing 100x or friction bought from the wrong stress are all far outside it.
			 *
			 * THE ZERO-COHESION ROW GETS A SECOND, TIGHTER STATEMENT, because sigma_n cancels out
			 * of it completely: its answer is (e - h/6)/z divided by mu, load-independent, and so
			 * it is also pinned as an identity rather than as a measurement.
			 */
			const double ExpectedDemandOverCapacity = Case.DemandOverCapacityAt28BrickWeights;

			if (Case.Strength.ShearCohesionMPa == 0.0)
			{
				const double Identity =
					ExpectedThrustPerReaction / Case.Strength.FrictionCoefficient;

				/*
				 * TO 1e-12 AND NOT WITH ==, and the reason is measured rather than fastidious:
				 * the row's hand figure, the identity built from the fixture's own three lengths
				 * and the ratio measured off the solver's force are three different orders of
				 * operation for one quantity, and they land one ulp apart (...921 against ...923).
				 * A tolerance twelve orders below that is still a bit-level claim about the
				 * arithmetic and not a licence for the physics to drift.
				 */
				TestTrue(
					FString::Printf(
						TEXT("%s, %s: with no cohesion the load cancels and the answer IS ")
						TEXT("(e - h/6)/z/mu = %s; the row's hand figure is %s and the fixture ")
						TEXT("measures %s"),
						Case.Description, Springing.Description, *Bits(Identity),
						*Bits(ExpectedDemandOverCapacity), *Bits(DemandOverCapacity)),
					FMath::Abs(DemandOverCapacity - Identity) <= 1.0e-12 * Identity
						&& FMath::Abs(ExpectedDemandOverCapacity - Identity) <= 1.0e-12 * Identity);
			}

			TestTrue(
				FString::Printf(
					TEXT("%s, %s: the implied push must be %s of this joint's sliding capacity, the ")
					TEXT("fixture measures %s"),
					Case.Description, Springing.Description, *Bits(ExpectedDemandOverCapacity),
					*Bits(DemandOverCapacity)),
				FMath::Abs(DemandOverCapacity - ExpectedDemandOverCapacity)
					<= 0.02 * ExpectedDemandOverCapacity);

			TestTrue(
				FString::Printf(
					TEXT("%s, %s: FIXTURE: the row claims the springing %s carry the thrust, and the ")
					TEXT("arithmetic says %s (%s of capacity)"),
					Case.Description, Springing.Description,
					Case.bCanCarryTheThrust ? TEXT("CAN") : TEXT("CANNOT"),
					DemandOverCapacity <= 1.0 ? TEXT("it can") : TEXT("it cannot"),
					*Bits(DemandOverCapacity)),
				(DemandOverCapacity <= 1.0) == Case.bCanCarryTheThrust);

			/*
			 * THE CLAIM. An arch whose springing cannot deliver the push is not an arch, so the
			 * joint must read OVER CAPACITY — on whichever axis the implementation puts the demand,
			 * which is why this is a threshold rather than a figure. Today every row reads
			 * 2|sigma_n|/f_c and stands: the relief is granted on topology alone.
			 */
			if (Case.bCanCarryTheThrust)
			{
				TestTrue(
					FString::Printf(
						TEXT("%s, %s: the springing CAN carry the thrust, so the relief is earned and ")
						TEXT("the joint must stay under capacity; it reads %s"),
						Case.Description, Springing.Description, *Bits(Utilisation)),
					Utilisation < 1.0);
			}
			else
			{
				TestTrue(
					FString::Printf(
						TEXT("%s, %s: the springing CANNOT carry the thrust — it is asked for %s of ")
						TEXT("its sliding capacity — so the one-cell relief is UNEARNED and the joint ")
						TEXT("must read over capacity; it reads %s"),
						Case.Description, Springing.Description, *Bits(DemandOverCapacity),
						*Bits(Utilisation)),
					Utilisation > 1.0);
			}

			SpringingSeatJoints.Add(SeatJoint);
			SpringingBricks.Add(Brick);
		}

		if (SpringingSeatJoints.Num() != 2)
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE: the cut must leave exactly two half-seated bricks, it left %d"),
				Case.Description, SpringingSeatJoints.Num()));

			continue;
		}

		/*
		 * AND THE OUTCOME, WHICH IS A COUNT OF JOINTS THAT FAILED UNDER LOAD.
		 *
		 * COUNTED BY BREAK PASS AND NOT BY HasGiven, AND THE TWO ARE GENUINELY DIFFERENT. A joint
		 * that went WITH A REMOVED PIECE has HasGiven true and a pass of INDEX_NONE, because it
		 * never snapped — the player's own click always takes six joints with it, so a HasGiven
		 * count can never reach zero and would make the earned row's claim unsatisfiable however
		 * correct the physics got.
		 *
		 * SolveLoads is non-destructive by contract, so everything read above is still intact and
		 * this runs on the same structure.
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

		int32 SpringingsLost = 0;

		for (const int32 SeatJoint : SpringingSeatJoints)
		{
			if (Cut.Structure.GetConnection(SeatJoint).HasGiven()
				&& Cut.Structure.GetBreakPass(SeatJoint) != INDEX_NONE)
			{
				++SpringingsLost;
			}
		}

		AddInfo(FString::Printf(
			TEXT("%s: the cascade ran %d passes; %d of %d joints failed under load, and %d of the 2 ")
			TEXT("springings were among them. The two half-seated bricks end %d and %d ")
			TEXT("(Falling %d, Supported %d, Stranded %d, Grounded %d)"),
			Case.Description, BreakingPasses, JointsBrokenByLoad, Cut.Structure.NumConnections(),
			SpringingsLost,
			static_cast<int32>(Cut.Structure.GetPieceSupport(SpringingBricks[0])),
			static_cast<int32>(Cut.Structure.GetPieceSupport(SpringingBricks[1])),
			static_cast<int32>(EPieceSupport::Falling),
			static_cast<int32>(EPieceSupport::Supported),
			static_cast<int32>(EPieceSupport::Stranded),
			static_cast<int32>(EPieceSupport::Grounded)));

		if (Case.bCanCarryTheThrust)
		{
			TestEqual(
				FString::Printf(
					TEXT("%s: the thrust is affordable, so one interior deletion must break NOTHING ")
					TEXT("under load; %d joints did over %d passes"),
					Case.Description, JointsBrokenByLoad, BreakingPasses),
				JointsBrokenByLoad, 0);
		}
		else
		{
			TestEqual(
				FString::Printf(
					TEXT("%s: the thrust is unaffordable, so BOTH unearned springings must fail under ")
					TEXT("load rather than being quietly relieved; %d of 2 did"),
					Case.Description, SpringingsLost),
				SpringingsLost, 2);

			/*
			 * AND THE PIECE COMES DOWN, WHICH IS THE OUTCOME RATHER THAN THE MECHANISM. A single
			 * severed joint is not a collapse — a wall can sever one and stand — so what is
			 * claimed is that the brick the unearned arch was holding up ends with no path to the
			 * ground at all.
			 *
			 * FALLING AND NOT MERELY "not Supported", because Stranded is a solver limitation
			 * wearing a collapse's clothes and DESIGN §4 requires collapse claims to say which
			 * one they got. Measured reachable: under a withhold-the-relief mutation this wall
			 * loses 59 joints over 7 passes and both bricks read Falling.
			 */
			for (const int32 Brick : SpringingBricks)
			{
				TestEqual(
					FString::Printf(
						TEXT("%s: the brick the unearned arch was holding up must FALL rather than ")
						TEXT("be stranded or left standing; piece %d reads %d"),
						Case.Description, Brick,
						static_cast<int32>(Cut.Structure.GetPieceSupport(Brick))),
					Cut.Structure.GetPieceSupport(Brick), EPieceSupport::Falling);
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
