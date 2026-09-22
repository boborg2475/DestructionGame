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
 * Named namespace, not anonymous: a unity build merges files, so identically-named helpers in two
 * anonymous namespaces would be a hard compile error.
 */
namespace StructureOneCellThrustTestSupport
{
	using namespace DestructionProfiles;
	using namespace StructureArchingTestSupport;

	/**
	 * One row of the table: the same wall, cut and one-cell arch, with a joint that either can or
	 * cannot deliver the arch's sideways push. Only the profile moves, so this is a statement about
	 * capacity, not shape.
	 */
	struct FThrustCase
	{
		const TCHAR* Description;

		FConnectionStrength Strength;

		/** Whether the springing can carry the horizontal push the moment cap assumes. */
		bool bCanCarryTheThrust;

		/**
		 * What fraction of its sliding capacity that push comes to, worked by hand at the 28 brick
		 * weights this cut leaves on the seat. Held per row so the fixture and the arithmetic must agree.
		 */
		double DemandOverCapacityAt28BrickWeights;
	};

	/**
	 * How hard a joint may be pushed sideways, MPa — Mohr-Coulomb `c + mu * sigma`, truncated at the
	 * profile's ceiling, written out rather than imported so it cannot agree with a wrong production
	 * expression. Compression is a magnitude: ReadBedJoint signs sigma_n positive in tension, and
	 * only a squeeze buys friction.
	 */
	inline double SlidingCapacityMPa(const FConnectionStrength& Strength, double NormalStressMPa)
	{
		return FMath::Min(
			Strength.ShearCohesionMPa + Strength.FrictionCoefficient * FMath::Abs(NormalStressMPa),
			Strength.MaxShearStrengthMPa);
	}
}

/**
 * The one-cell arching relief has to be earned: the springing must carry the sideways push the
 * moment cap assumes, and a joint that cannot must not be granted it.
 *
 * What the cap assumes, derived from the cap itself. Delete one brick from a running-bond wall and
 * the brick above keeps one 10.25 x 10.25 seat, its load arriving e = 5.625 cm outside the patch
 * centroid. ArchingMomentScale caps the moment by k = |sigma_n| / sigma_b, i.e. M' = N * h/6 with
 * h/6 = 1.7083 cm — moving the thrust line from 5.625 cm out to the kern edge. That is what an arch
 * is, correct physics (DESIGN §5.4) when the abutments can take the thrust. Taking moments about
 * the seat centroid, the couple the cap deletes is dM = F*(e - h/6) = F * 3.9166667 cm, suppliable
 * only by a horizontal pair: a push H through the intact head joint and its reaction as shear in
 * the bed plane. With the measured arm z = 3.75 cm, H/V = (e - h/6)/z = 3.9166667/3.75 = 1.0444444,
 * load-independent — the one-cell analogue of the spanned case's H/V = 3L/(4*d_e).
 *
 * Today that push is neither applied nor checked: ApplyArchingThrust runs only over arches
 * ReseatSpannedGroups records, and a one-cell hole leaves nobody seatless, so the relief is granted
 * fail-open on topology alone (DESIGN §7 gap 4).
 *
 * The table: one wall (the 7 x 30 flush wall StructureArchingTest anchors at 0.0141885) with one
 * interior brick cut from course 1, laid in three profiles. At 28 brick weights the surviving seat
 * carries sigma_n = 0.0710835 MPa, so the push is 1.0444444 * 0.0710835 = 0.0742428 MPa:
 *   - General purpose mortar (c = 0.9, mu = 0.75): capacity 0.9533 MPa, demand 0.0779 of it. Earned.
 *   - Lime mortar (c = 0.27, mu = 0.75): capacity 0.3233 MPa, demand 0.2296 of it. Earned. (Its old
 *     anti-over-withhold bite now lives in StructureArchingTest's kern-edge identity, see CURRENT_STATE.)
 *   - Dry stone (c = 0, mu = 0.7): cohesion zero, so sigma_n cancels — 1.0444444/0.7 = 1.4920635 of
 *     capacity at every load and height. Not earned, the same sentence DESIGN §5.4 writes for the
 *     spanned case (0.866/0.7 = 1.2372). The one-cell hole is the case that sentence does not reach.
 *
 * No zero-cohesion-with-tensile-bond row: CohesionlessBond entangles gap 5 (composite vertical
 * action re-sections the moment to 0.807) behind gap 4, so a row needing both closed would mislead.
 *
 * What is asserted:
 *   - The mechanism, on the named seat: an unearned springing reads over capacity, an earned one
 *     under. Today the unearned rows read the arched compression answer 0.0047389 — the defect.
 *   - The outcome: joints that failed under load, counted by break pass (never HasGiven, since the
 *     player's deletion always takes six unsnapped joints with it), and the held-up bricks ending
 *     Falling rather than stranded or standing.
 *   - Never a displacement: two pieces can sever their bond and stay put.
 *   - Never which mechanism closes the gate: applying the thrust as a real shear demand and
 *     withholding the relief are both answers; the one thing forbidden is today's nothing.
 *
 * Which axis governs is worked for every row: ComputeUtilisation returns the worst of compression,
 * shear and tension. Earned rows are compression-governed (tension and shear zero at the cap); the
 * claim is written as "over capacity" not a figure on a nominated axis, and every axis is printed.
 *
 * No ticking world: FStructure is plain arithmetic over a graph; the 980 is the solver's constant.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureOneCellThrustTest,
	"DestructionGame.Core.Structure.AOneCellArchMustEarnItsThrust",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureOneCellThrustTest::RunTest(const FString& Parameters)
{
	using namespace StructureOneCellThrustTestSupport;
	using namespace StaircaseWallTestSupport;

	/* Expected numbers are ratios of published strengths, asserted not imported: reading the profile
	 * back would agree with a wrong profile. */
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
	 * Lime's mean-basis row (re-anchor 2026-08-13): tensile 0.20 is the NHL 2 bond-wrench mean at 6
	 * months, cohesion 0.27 is the mean shear/flexural ratio 1.34 x 0.20 (Gooch et al. 2023), and
	 * compressive 2.0 is a class-floor choice that does not move.
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
	 * The arm the cap's couple acts over, a fixture expectation measured off each joint below. Half a
	 * brick plus half a mortar joint is the rise from a bed plane to the head joint centroid above.
	 */
	constexpr double ExpectedThrustArmCm = BrickHeightCm / 2.0 + MortarJointCm / 2.0;

	/** (5.625 - 1.7083333) / 3.75, and the load has cancelled out of it. */
	constexpr double ExpectedThrustPerReaction = 1.0444444444444445;

	const TArray<FThrustCase> Cases = {
		/*
		 * The anchor's own wall: a mortared springing asked for 0.0742 MPa against 0.9533 MPa of bond
		 * plus friction — 0.0779 of capacity — so the relief is granted. The fixture StructureArchingTest
		 * pins at 0.0141885, re-entered so a gate that closed on everything fails here too.
		 */
		{ TEXT("EARNED: general purpose mortar, c = 0.9 + 0.75 sigma"), GeneralPurposeMortar,
			true, 0.077879 },

		/*
		 * Lime: a third of the cohesion, so the push costs 0.0742 MPa against 0.3233 MPa — 0.2296 of
		 * capacity, still earned. Its old anti-over-withhold bite now lives in StructureArchingTest's
		 * kern-edge identity (see CURRENT_STATE).
		 */
		{ TEXT("EARNED: lime mortar, c = 0.27 + 0.75 sigma"), LimeMortar, true, 0.229632 },

		/*
		 * Dry stone, where the push is the only thing that can fail: cohesion is exactly zero, so
		 * sigma cancels — 1.4921 of capacity at every load and height, the shape of the spanned case's
		 * 0.866/0.7 = 1.2372 (DESIGN §5.4).
		 */
		{ TEXT("UNEARNED: dry stone, c = 0, mu = 0.7"), DryStone, false, 1.4920634920634921 },
	};

	for (const FThrustCase& Case : Cases)
	{
		/*
		 * The same wall and cut as the arching anchor — 7 x 30 flush, third full brick of course 1 —
		 * profile the only difference; the spec comes from the shared header, not a second definition.
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

		/* The wall must stand before it is cut, or a deletion row measures the material instead.
		 * Every seat of an intact running bond has e = 0, so nothing carries a moment here. */
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
		 * The two bricks the deletion left on half a seat, each overhanging toward the other, so the
		 * head joint between them is on each one's eccentric side and each is the other's abutment —
		 * the one-cell arch.
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

			/* Gate four, as a precondition: the abutment reaches the ground on its own account.
			 * Without that there is no arch to earn, and the row would measure a refused cap. */
			TestTrue(
				FString::Printf(
					TEXT("%s, %s: FIXTURE: the abutment must be Supported or Grounded on its own ")
					TEXT("account, and the half-seated brick must still be Supported"),
					Case.Description, Springing.Description),
				(Cut.Structure.GetPieceSupport(Abutment) == EPieceSupport::Supported
					|| Cut.Structure.GetPieceSupport(Abutment) == EPieceSupport::Grounded)
					&& Cut.Structure.GetPieceSupport(Brick) == EPieceSupport::Supported);

			/* The geometry the thrust is built from, all three lengths measured off the fixture: the
			 * overhang, the seat's kern, and the arm up to the head joint the push arrives through. */
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

			/* And the push itself, load cancelled: dM = F*(e - h/6) is the couple the cap deletes and
			 * H*z the only thing that can supply it, so H/V is a fact about the bond alone. */
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

			/* What the springing is asked for against what it can give, in MPa, built from the
			 * solver's reported force: only the force comes from production. */
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

			/* And what the head joint the cap leans on carries. Reported not asserted: whether the
			 * thrust travels this joint or is only checked against capacity is the implementation's choice. */
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
			 * The row's arithmetic, asserted before its verdict, to 2% of the hand figure — slack
			 * for the real load being 27.94 brick weights, not exactly 28; a wrong arm, kern or
			 * friction is far outside it. The zero-cohesion row also gets a tighter identity check,
			 * since sigma_n cancels and its answer is (e - h/6)/z/mu, load-independent.
			 */
			const double ExpectedDemandOverCapacity = Case.DemandOverCapacityAt28BrickWeights;

			if (Case.Strength.ShearCohesionMPa == 0.0)
			{
				const double Identity =
					ExpectedThrustPerReaction / Case.Strength.FrictionCoefficient;

				/* To 1e-12, not ==: the hand figure, the identity from the fixture's lengths, and
				 * the ratio off the solver's force are three orders of operation for one quantity
				 * and land one ulp apart — still a bit-level claim, not licence to drift. */
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
			 * joint reads over capacity — on whichever axis, which is why this is a threshold. */
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
		 * And the outcome: joints that failed under load, counted by break pass not HasGiven. A joint
		 * that went with a removed piece has HasGiven true and pass INDEX_NONE, so a HasGiven count
		 * never reaches zero and would make the earned row unsatisfiable. SolveLoads is non-destructive,
		 * so this runs on the same structure.
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
			 * And the piece comes down — outcome, not mechanism. The brick the unearned arch held
			 * up ends with no path to the ground: Falling, not merely "not Supported", because
			 * Stranded is a solver limitation and DESIGN §4 requires collapse claims to say which.
			 * Measured reachable: a withhold-the-relief mutation loses 59 joints over 7 passes and
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
