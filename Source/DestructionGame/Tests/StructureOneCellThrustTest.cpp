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

	/** One row: the same wall, cut and arch; only the joint profile varies. */
	struct FThrustCase
	{
		const TCHAR* Description;

		FConnectionStrength Strength;

		/** Whether the springing can carry the horizontal push the moment cap assumes. */
		bool bCanCarryTheThrust;

		/** The push as a fraction of sliding capacity, worked by hand at 28 brick weights on the seat. */
		double DemandOverCapacityAt28BrickWeights;
	};

	/**
	 * Sliding capacity, MPa: Mohr-Coulomb c + mu * |sigma|, capped; written out, not imported.
	 * Callers pass compressive sigma_n (ReadBedJoint signs tension positive).
	 */
	inline double SlidingCapacityMPa(const FConnectionStrength& Strength, double NormalStressMPa)
	{
		return FMath::Min(
			Strength.ShearCohesionMPa + Strength.FrictionCoefficient * FMath::Abs(NormalStressMPa),
			Strength.MaxShearStrengthMPa);
	}
}

/**
 * One-cell arching relief must be earned: the springing must carry the sideways push the moment cap
 * assumes (DESIGN §5.4, §7 gap 4).
 *
 * Delete one brick and the brick above keeps one 10.25 x 10.25 seat with its load e = 5.625 cm off
 * centre. The cap moves the thrust line to the kern edge (h/6 = 1.7083 cm). The couple removed,
 * F*(e - h/6), can only come from a horizontal push through the head joint, arm z = 3.75 cm, so
 * H/V = (e - h/6)/z = 1.0444444 regardless of load.
 *
 * Three profiles on the 7 x 30 flush wall, course-1 interior cut. At 28 brick weights the seat has
 * sigma_n = 0.0710835 MPa, so the push is 0.0742428 MPa:
 *   - General purpose mortar (c = 0.9, mu = 0.75): 0.0779 of capacity. Earned.
 *   - Lime mortar (c = 0.27, mu = 0.75): 0.2296 of capacity. Earned.
 *   - Dry stone (c = 0, mu = 0.7): sigma_n cancels, 1.0444444/0.7 = 1.4920635 at any load. Not earned.
 *
 * No zero-cohesion-with-tensile-bond row: it would also depend on gap 5 being closed.
 *
 * Asserts the seat reads over capacity when unearned and under when earned (on the worst axis, all
 * axes printed); joints broken by load counted by break pass, not HasGiven (the deletion severs six
 * joints itself); and unearned springings end Falling. Never displacement, and never which fix
 * (applying the thrust or withholding the relief) closes the gap.
 *
 * No ticking world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureOneCellThrustTest,
	"DestructionGame.Core.Structure.AOneCellArchMustEarnItsThrust",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureOneCellThrustTest::RunTest(const FString& Parameters)
{
	using namespace StructureOneCellThrustTestSupport;
	using namespace StaircaseWallTestSupport;

	// Profile strengths asserted, not imported, so a wrong profile fails.
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
	 * Lime, mean basis: tensile 0.20 (NHL 2 bond wrench, 6 months), cohesion 1.34 x 0.20 = 0.27
	 * (Gooch et al. 2023), compressive 2.0 (class floor).
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

	/** Rise from the bed plane to the head joint centroid: half a brick plus half a joint. */
	constexpr double ExpectedThrustArmCm = BrickHeightCm / 2.0 + MortarJointCm / 2.0;

	/** (5.625 - 1.7083333) / 3.75. */
	constexpr double ExpectedThrustPerReaction = 1.0444444444444445;

	const TArray<FThrustCase> Cases = {
		// StructureArchingTest's anchor wall; catches a gate that refuses everything.
		{ TEXT("EARNED: general purpose mortar, c = 0.9 + 0.75 sigma"), GeneralPurposeMortar,
			true, 0.077879 },

		{ TEXT("EARNED: lime mortar, c = 0.27 + 0.75 sigma"), LimeMortar, true, 0.229632 },

		// Zero cohesion: sigma cancels, so the ratio is the same at any load.
		{ TEXT("UNEARNED: dry stone, c = 0, mu = 0.7"), DryStone, false, 1.4920634920634921 },
	};

	for (const FThrustCase& Case : Cases)
	{
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

		// The intact wall must stand, or the cut rows would measure the material rather than the cut.
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

		/** The two half-seated bricks, each the other's abutment: the one-cell arch. */
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

			// Precondition: the abutment stands on its own, or there is no arch to earn.
			TestTrue(
				FString::Printf(
					TEXT("%s, %s: FIXTURE: the abutment must be Supported or Grounded on its own ")
					TEXT("account, and the half-seated brick must still be Supported"),
					Case.Description, Springing.Description),
				(Cut.Structure.GetPieceSupport(Abutment) == EPieceSupport::Supported
					|| Cut.Structure.GetPieceSupport(Abutment) == EPieceSupport::Grounded)
					&& Cut.Structure.GetPieceSupport(Brick) == EPieceSupport::Supported);

			// Overhang, kern and thrust arm, measured off the fixture.
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

			// H*z must supply the removed couple F*(e - h/6), so H/V depends on geometry only.
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

			// Demand and capacity, MPa; only the force comes from production.
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

			// Head-joint force is reported, not asserted: routing the thrust through it is optional.
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
			 * The row's arithmetic, to 2% (the real load is 27.94 brick weights, not 28). The
			 * zero-cohesion row also checks the exact identity (e - h/6)/z/mu.
			 */
			const double ExpectedDemandOverCapacity = Case.DemandOverCapacityAt28BrickWeights;

			if (Case.Strength.ShearCohesionMPa == 0.0)
			{
				const double Identity =
					ExpectedThrustPerReaction / Case.Strength.FrictionCoefficient;

				// 1e-12, not ==: three orders of operation land one ulp apart.
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

			// The claim: an unearned springing reads over capacity on whichever axis.
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
		 * Outcome: joints broken by load, counted by break pass. Joints severed with the removed piece
		 * have HasGiven true but pass INDEX_NONE, so a HasGiven count could never reach zero.
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
			 * The held-up brick must read Falling, not just "not Supported" (Stranded is a solver
			 * limitation; DESIGN §4). A withhold-the-relief mutation reaches this: 59 joints over 7 passes.
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
