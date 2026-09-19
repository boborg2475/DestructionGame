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
	 * One row of the table: the same wall, the same cut, the same one-cell arch — and a joint
	 * that either can or cannot deliver the sideways push that arch is made of.
	 *
	 * The profile is the only thing that moves: geometry decides how hard the arch pushes and
	 * the profile decides what the springing can take, so this is a statement about CAPACITY
	 * rather than about shape.
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
	 * How hard a joint may be pushed sideways, MPa — Mohr-Coulomb, written out here rather than
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
 * WHAT THE CAP ASSUMES, derived from the cap itself rather than from BS 5977. Delete one brick
 * from a running-bond wall and the brick above keeps exactly one 10.25 x 10.25 seat, with its
 * load arriving e = 5.625 cm outside that patch's centroid. `ArchingMomentScale` caps the whole
 * moment vector by k = |sigma_n| / sigma_b, the same statement as
 *
 *     M' = k * M = (N/A) * W = N * h/6        h/6 = 10.25/6 = 1.7083333 cm
 *
 * — the thrust line moves from 5.625 cm out to the kern edge at 1.7083 cm out. That is what an
 * arch IS, and DESIGN §5.4 says it is correct physics WHEN THE ABUTMENTS CAN TAKE THE THRUST.
 * What it is not is free. Taking moments about the seat's own centroid on the free body of the
 * half-seated brick:
 *
 *     F * e  (overturning)  =  M' (what the joint keeps)  +  H * z (a couple something supplies)
 *
 * so the couple the cap deletes is dM = F * (e - h/6) = F * 3.9166667 cm, and nothing on that
 * free body can supply it except a horizontal pair: a push H from the abutment through the intact
 * head joint, and its reaction as shear in the seat's own bed plane. The arm z between them is
 * measured, not assumed — the head joint's centroid above the bed joint's, 3.75 cm for this brick
 * and mortar — so H/V = (e - h/6)/z = 3.9166667/3.75 = 1.0444444, with F cancelled out: a fact
 * about the bond geometry alone, at every wall height and every load, exactly as the spanned
 * case's H/V = 3L/(4*d_e) is.
 *
 * TODAY THAT PUSH IS NEITHER APPLIED NOR CHECKED. `ApplyArchingThrust` only ever runs over arches
 * `ReseatSpannedGroups` records, and a one-cell hole leaves nobody seatless, so no group forms, no
 * arch is recorded, and the springing's force vector stays (0, 0, -F) exactly — DESIGN §7 gap 4.
 * The relief is granted fail-open, on topology alone.
 *
 * SENSITIVITY OF THE VERDICTS TO THE ARM. The only free choice in the derivation is where inside
 * the head joint's face the push acts: at its centroid (H/V = 1.0444), at its own kern edge
 * (H/V = 0.8103), or by the model's spanned formula read at one cell (H/V = 0.8660). Every row
 * below reads the same verdict under all three; the arm that would flip the dry-stone row is
 * 3.9166667/0.7 = 5.5952 cm, 1.85 cm past the head joint's centroid. The assertions use the
 * measured 3.75 cm arm.
 *
 * THE TABLE. One wall — the 7 x 30 flush wall `StructureArchingTest` anchors at 0.0141885 — with
 * one interior brick cut from course 1, laid three times in three profiles. At 28 brick weights
 * the surviving seat carries sigma_n = 74681.56 / (105.0625 * 10000) = 0.0710835 MPa, so the push
 * it must deliver is 1.0444444 * 0.0710835 = 0.0742428 MPa:
 *
 *   GENERAL PURPOSE MORTAR (mean basis, c = 0.9, mu = 0.75). Capacity 0.9533126 MPa, demand
 *     0.0779 of it — a margin of 12.8. THE ARCH IS EARNED. The margin is not load-independent —
 *     cohesion is constant while demand is linear — but mean-cohesion mortar only runs out past
 *     sigma_n ~ 2 MPa, hundreds of brick weights beyond anything this project builds.
 *
 *   LIME MORTAR (mean basis, c = 0.27, mu = 0.75). Capacity 0.3233126 MPa, demand 0.2296 of it —
 *     still earned. On the OLD characteristic basis this row was the anti-over-withhold gate
 *     (withholding everywhere left cement standing at 0.646 while lime's weaker bond read ~1.29
 *     in tension and came down); at mean strengths lime's un-arched tension is ~0.32 and stands
 *     either way, so that bite now lives in StructureArchingTest's kern-edge identity instead — a
 *     lime-specific arched-reading pin here is the specified replacement, see CURRENT_STATE.
 *
 *   DRY STONE (c = 0.0, mu = 0.7). Zero cohesion, so sigma_n cancels twice over: 1.0444444/0.7 =
 *     1.4920635 of capacity AT EVERY LOAD AND EVERY WALL HEIGHT. THE ARCH IS NOT EARNED — the
 *     same sentence DESIGN §5.4 already writes for the spanned case (0.866/0.7 = 1.2372): you
 *     cannot span a hole in a dry-stone wall with a flat arch. The one-cell hole is the case that
 *     sentence does not currently reach.
 *
 * WHY THERE IS NO ZERO-COHESION ROW WITH A TENSILE BOND — the row this table wanted and cannot
 * have. `CohesionlessBond` (dry stone's friction with a real 0.08 MPa tensile bond) entangles two
 * gaps instead of isolating this one: withhold the unearned relief and the joint does not fail,
 * because composite vertical action re-sections its moment and reads 0.807 — DESIGN §7 gap 5 (the
 * deep beam's horizontal shear flow is never applied as a demand either) standing behind gap 4. A
 * row satisfiable only by closing both would send the wrong instruction.
 *
 * WHAT IS ASSERTED, AND WHY IN THAT FORM:
 *
 *   - THE MECHANISM, on the named seat: an unearned springing must read OVER CAPACITY and an
 *     earned one must read under it. Today the unearned rows read 2*0.0710835/30 = 0.0047389 —
 *     the arched compression answer, and the whole defect.
 *
 *   - THE OUTCOME, as a count of joints that FAILED UNDER LOAD after a cascade, and as where the
 *     brick ends up. A single severed joint is not a collapse, so the earned rows claim nothing
 *     failed under load, and the unearned row claims both unearned springings did — with the two
 *     bricks they held up ending FALLING rather than stranded or standing. Counted by break pass
 *     and never by `HasGiven`: the player's own deletion always takes six joints with it and
 *     those never snapped.
 *
 *   - NEVER A DISPLACEMENT: two pieces can sever their bond and stay resting exactly where they
 *     were, so how far anything travelled says nothing about the mortar.
 *
 *   - AND NEVER WHICH MECHANISM CLOSES THE GATE. Applying the thrust as a real shear demand and
 *     withholding the relief where the springing cannot carry it are both answers to this test:
 *     the unearned seat reads 1.4920635 in shear under the first and its un-arched tension (Max,
 *     for dry stone's exact zero) under the second. Both are over capacity and both leave the
 *     earned rows standing. The one thing neither may do is what happens today: nothing at all.
 *
 * WHICH AXIS GOVERNS, worked for every row before anything is claimed. `ComputeUtilisation`
 * returns the worst of compression, shear and tension, so a fixture aimed at sliding silently
 * measures something else the moment something else is higher. On the earned rows compression
 * governs today at 2|sigma_n|/f_c (0.0142 cement, 0.0709 lime; tension is exactly zero at the cap
 * and shear is exactly zero since no thrust is applied); on the unearned row compression governs
 * at 0.0047. The claim is written as "over capacity" rather than a figure on a nominated axis,
 * and every axis is printed.
 *
 * NEEDS A TICKING WORLD: no. `FStructure` is plain arithmetic over a graph; the 980 is the
 * solver's own constant, not a physics scene's.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureOneCellThrustTest,
	"DestructionGame.Core.Structure.AOneCellArchMustEarnItsThrust",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureOneCellThrustTest::RunTest(const FString& Parameters)
{
	using namespace StructureOneCellThrustTestSupport;
	using namespace StaircaseWallTestSupport;

	/* The expected numbers are ratios of published strengths, asserted rather than imported: a
	 * test that read the profile back would agree with a wrong profile. */
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean mortar cohesion 0.9 MPa (re-anchor 2026-08-13), the profile carries %g"),
			GeneralPurposeMortar.ShearCohesionMPa),
		GeneralPurposeMortar.ShearCohesionMPa == 0.9);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean mortar friction 0.75, the profile carries %g"),
			GeneralPurposeMortar.FrictionCoefficient),
		GeneralPurposeMortar.FrictionCoefficient == 0.75);

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

	/*
	 * Lime's mean-basis row (re-anchor 2026-08-13): tensile 0.20 is the measured NHL 2 bond-
	 * wrench mean at 6 months (the profile's 2.0 MPa compressive IS NHL 2); cohesion 0.27 is
	 * the campaign's mean shear/flexural ratio 1.34 x 0.20 (Gooch et al. 2023). Compressive
	 * 2.0 is a class-floor choice and does not move.
	 */
	TestTrue(
		FString::Printf(
			TEXT("FIXTURE: derived against mean lime cohesion 0.27 MPa, tensile 0.20 MPa and compressive ")
			TEXT("2.0 MPa; the profile carries %g, %g and %g"),
			LimeMortar.ShearCohesionMPa, LimeMortar.TensileStrengthMPa,
			LimeMortar.CompressiveStrengthMPa),
		LimeMortar.ShearCohesionMPa == 0.27 && LimeMortar.TensileStrengthMPa == 0.2
			&& LimeMortar.CompressiveStrengthMPa == 2.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == 1.9);

	/**
	 * The arm the cap's couple acts over, as a fixture expectation rather than an input: it is
	 * measured off each joint below and compared against this. Half a brick plus half a mortar
	 * joint is the rise from a bed plane to the head joint centroid in the course above it.
	 */
	constexpr double ExpectedThrustArmCm = BrickHeightCm / 2.0 + MortarJointCm / 2.0;

	/** (5.625 - 1.7083333) / 3.75, and the load has cancelled out of it. */
	constexpr double ExpectedThrustPerReaction = 1.0444444444444445;

	const TArray<FThrustCase> Cases = {
		/*
		 * The anchor's own wall: a mortared springing is asked for 0.0742 MPa of sliding against
		 * 0.9533 MPa of bond plus friction — 0.0779 of capacity, a margin of 12.8 — so the relief
		 * is correctly granted. This is the fixture `StructureArchingTest` pins at 0.0141885,
		 * re-entered here so a gate that closed on everything fails in the file that closed it.
		 */
		{ TEXT("EARNED: general purpose mortar, c = 0.9 + 0.75 sigma"), GeneralPurposeMortar,
			true, 0.077879 },

		/*
		 * Lime: a third of the cohesion, so the push costs 0.0742 MPa against 0.3233 MPa — 0.2296
		 * of capacity, comfortably affordable, so the relief is still earned. On the old
		 * characteristic basis this row was the anti-over-withhold gate (wholesale withholding
		 * failed it at ~1.29 in tension while cement survived); at the mean basis its un-arched
		 * tension is ~0.32 and stands either way, so that bite now lives in
		 * StructureArchingTest's kern-edge identity — the specified replacement is recorded in
		 * CURRENT_STATE.
		 */
		{ TEXT("EARNED: lime mortar, c = 0.27 + 0.75 sigma"), LimeMortar, true, 0.229632 },

		/*
		 * Dry stone, where the push is the only thing that can fail: cohesion is an exact zero,
		 * so capacity is 0.7*sigma against a demand of 1.0444*sigma and sigma cancels — 1.4921 of
		 * capacity at every load, wall height and brick density, the same shape as the spanned
		 * case's 0.866/0.7 = 1.2372 DESIGN §5.4 already states outright.
		 */
		{ TEXT("UNEARNED: dry stone, c = 0, mu = 0.7"), DryStone, false, 1.4920634920634921 },
	};

	for (const FThrustCase& Case : Cases)
	{
		/*
		 * The same wall and cut as the arching anchor — 7 x 30, flush, the third full brick of
		 * course 1 — with the profile as the only difference; the spec comes from the shared
		 * header rather than a second definition that could drift.
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

		/* The wall has to stand before it is cut, or a row about one deletion measures the
		 * material instead. Every seat of an intact running bond has e = 0 exactly, so nothing
		 * here carries a moment whatever the profile is. */
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

			/* Gate four, as a fixture precondition: the abutment reaches the ground on its own
			 * account. Without that there is no arch to earn, and the row below would be
			 * measuring a refused cap rather than an unearned one. */
			TestTrue(
				FString::Printf(
					TEXT("%s, %s: FIXTURE: the abutment must be Supported or Grounded on its own ")
					TEXT("account, and the half-seated brick must still be Supported"),
					Case.Description, Springing.Description),
				(Cut.Structure.GetPieceSupport(Abutment) == EPieceSupport::Supported
					|| Cut.Structure.GetPieceSupport(Abutment) == EPieceSupport::Grounded)
					&& Cut.Structure.GetPieceSupport(Brick) == EPieceSupport::Supported);

			/* The geometry the implied thrust is built from, all three lengths measured off the
			 * fixture rather than assumed: the overhang, the seat's own kern, and the arm from
			 * the bed plane up to the head joint the push has to arrive through. */
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

			/* And the push itself, with the load cancelled out of it: dM = F*(e - h/6) is the
			 * couple the cap deletes and H*z is the only thing on this free body that can supply
			 * it, so H/V is a fact about the bond alone — the same shape as 3L/(4*d_e). */
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

			/* What the springing is asked for against what it can give, both in MPa and built
			 * here from the solver's own reported force: only the force comes from production. */
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

			/* And what the head joint the cap leans on is carrying — the defect in one number.
			 * Reported rather than asserted: whether the thrust travels through this joint or is
			 * only checked against the springing's capacity is the implementation's choice. */
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
			 * The arithmetic of the row, asserted before its verdict, to two percent of the hand
			 * figure — slack for the wall's real load distribution being 27.94 brick weights
			 * rather than exactly 28, and nothing else; a wrong lever arm, kern or friction is far
			 * outside it.
			 *
			 * The zero-cohesion row gets a second, tighter statement, because sigma_n cancels out
			 * completely: its answer is (e - h/6)/z divided by mu, load-independent, so it is also
			 * pinned as an identity rather than a measurement.
			 */
			const double ExpectedDemandOverCapacity = Case.DemandOverCapacityAt28BrickWeights;

			if (Case.Strength.ShearCohesionMPa == 0.0)
			{
				const double Identity =
					ExpectedThrustPerReaction / Case.Strength.FrictionCoefficient;

				/* To 1e-12 and not with ==: the row's hand figure, the identity built from the
				 * fixture's own three lengths, and the ratio measured off the solver's force are
				 * three different orders of operation for one quantity and land one ulp apart
				 * (...921 against ...923) — this tolerance is still a bit-level claim, not a
				 * licence for the physics to drift. */
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

			/* The claim: an arch whose springing cannot deliver the push is not an arch, so the
			 * joint must read over capacity — on whichever axis the implementation puts the
			 * demand, which is why this is a threshold rather than a figure. */
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
		 * And the outcome: a count of joints that failed under load. Counted by break pass and
		 * not by HasGiven — a joint that went WITH A REMOVED PIECE has HasGiven true and a pass
		 * of INDEX_NONE, because it never snapped, so a HasGiven count can never reach zero and
		 * would make the earned row's claim unsatisfiable however correct the physics got.
		 * SolveLoads is non-destructive by contract, so this runs on the same structure.
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
			 * And the piece comes down — the outcome rather than the mechanism. A single severed
			 * joint is not a collapse, so what is claimed is that the brick the unearned arch was
			 * holding up ends with no path to the ground at all. FALLING and not merely "not
			 * Supported", because Stranded is a solver limitation wearing a collapse's clothes
			 * and DESIGN §4 requires collapse claims to say which one they got. Measured
			 * reachable: a withhold-the-relief mutation loses 59 joints over 7 passes here and
			 * both bricks read Falling.
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
