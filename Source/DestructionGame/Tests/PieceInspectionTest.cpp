// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceInspection.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Named, uniquely-named namespace: unity builds merge files, so anonymous-namespace names can collide. */
namespace PieceInspectionTestSupport
{
	using namespace DestructionProfiles;

	/**
	 * Gravity, transcribed rather than imported. Already in uu: 2 kg weighs 2 x 980 = 1960 uu
	 * (19.6 N x 100), so any further factor of 100 is a 100x error.
	 */
	constexpr double InspectionGravityCmPerSecondSquared = 980.0;

	/**
	 * Spelled out independently of production: 1 MPa over 1 cm2 is 100 N = 100 x 100 uu. If
	 * ForceUnitsPerMPaSqCm is ever wrong, this file fails rather than agreeing.
	 */
	constexpr double InspectionForceUnitsPerMPaSqCm = 100.0 * 100.0;

	/** An ordinary bed- or head-joint face. */
	constexpr double InspectionJointAreaSqCm = 100.0;

	/** The fixtures' structure id, and a different one. */
	constexpr int32 ThisStructure = 4;
	constexpr int32 SomeOtherStructure = 5;

	/*
	 * The worked fixture: five pieces, so the subject carries every role and one gone joint:
	 *
	 *                       [3] Rider  3 kg
	 *                        |  conn 2   bed joint ABOVE the subject
	 *      Pulled [4] ~ ~ ~ [1] Subject 2 kg ---- conn 1 ---- [2] Hanger 1 kg
	 *      (removed) conn 3  |                  head joint     (hangs off the side)
	 *                        |  conn 0   bed joint BENEATH the subject
	 *                       [0] Pad    grounded
	 *
	 * Piece 4 is removed before the solve, severing conn 3 without failing it: HasGiven true,
	 * no break pass ("went with a removed piece", DESIGN.md).
	 */
	constexpr int32 PadPiece = 0;
	constexpr int32 SubjectPiece = 1;
	constexpr int32 HangerPiece = 2;
	constexpr int32 RiderPiece = 3;
	constexpr int32 PulledPadPiece = 4;

	constexpr int32 PadJoint = 0;
	constexpr int32 HangerJoint = 1;
	constexpr int32 RiderJoint = 2;
	constexpr int32 PulledJoint = 3;

	constexpr double PadMassKg = 10.0;
	constexpr double SubjectMassKg = 2.0;
	constexpr double HangerMassKg = 1.0;
	constexpr double RiderMassKg = 3.0;
	constexpr double PulledPadMassKg = 7.0;

	/** Vertical normal: a bed joint, bearing in compression. */
	const FVector InspectionBedNormal(0.0, 0.0, 1.0);

	/** Horizontal normal: a head joint, carrying vertical load in shear. */
	const FVector InspectionHeadNormal(1.0, 0.0, 0.0);

	FPieceRef MakeRef(int32 StructureId, int32 PieceIndex)
	{
		FPieceRef Ref;
		Ref.StructureId = StructureId;
		Ref.PieceIndex = PieceIndex;
		return Ref;
	}

	/** A distinct box per handle, so nothing here can pass on a shifted array. */
	DestructionLayout::FPieceBox InspectionBoxFor(int32 Index)
	{
		DestructionLayout::FPieceBox Box;
		Box.CentreCm = FVector(Index * 100.0 + 1.0, 2.0, 3.0);
		Box.ExtentCm = FVector(10.75, 5.125, 3.25);
		return Box;
	}

	void AddJoint(
		FStructureBinding& Out,
		int32 PieceA,
		int32 PieceB,
		const FVector& Normal,
		double AreaSqCm)
	{
		FConnection Connection;
		Connection.PieceA = PieceA;
		Connection.PieceB = PieceB;
		Connection.InterfaceNormal = Normal;
		Connection.InterfaceAreaSqCm = AreaSqCm;
		Connection.Strength = GeneralPurposeMortar;
		Out.AddConnection(Connection);
	}

	/**
	 * Build the fixture above. Solving is optional because "not yet solved" is a state the readout
	 * must distinguish. Null actors are fine: nothing here touches an actor.
	 */
	void BuildWorkedFixture(FStructureBinding& Out, bool bPullThePad, bool bSolve)
	{
		Out.StructureId = ThisStructure;

		Out.AddPiece(PadMassKg, /*bIsGrounded*/ true, nullptr, InspectionBoxFor(PadPiece));
		Out.AddPiece(SubjectMassKg, false, nullptr, InspectionBoxFor(SubjectPiece));
		Out.AddPiece(HangerMassKg, false, nullptr, InspectionBoxFor(HangerPiece));
		Out.AddPiece(RiderMassKg, false, nullptr, InspectionBoxFor(RiderPiece));
		Out.AddPiece(PulledPadMassKg, /*bIsGrounded*/ true, nullptr, InspectionBoxFor(PulledPadPiece));

		AddJoint(Out, PadPiece, SubjectPiece, InspectionBedNormal, InspectionJointAreaSqCm);
		AddJoint(Out, SubjectPiece, HangerPiece, InspectionHeadNormal, InspectionJointAreaSqCm);
		AddJoint(Out, SubjectPiece, RiderPiece, InspectionBedNormal, InspectionJointAreaSqCm);
		AddJoint(Out, PulledPadPiece, SubjectPiece, InspectionBedNormal, InspectionJointAreaSqCm);

		if (bPullThePad)
		{
			Out.RemovePiece(PulledPadPiece);
		}

		if (bSolve)
		{
			Out.SolveLoads();
		}
	}

	/**
	 * Utilisation of a joint loaded only in compression (force antiparallel to a vertical normal,
	 * so shear and tension are exactly zero and cannot govern).
	 */
	double CompressionOnlyUtilisation(
		double ForceMagnitudeUu, double AreaSqCm, const FConnectionStrength& Strength)
	{
		const double StressMPa = ForceMagnitudeUu / (AreaSqCm * InspectionForceUnitsPerMPaSqCm);
		return StressMPa / Strength.CompressiveStrengthMPa;
	}

	/**
	 * Utilisation of a joint loaded only in shear with zero compression, so Mohr-Coulomb capacity
	 * is bare cohesion. Valid while the shear cap is above cohesion (asserted in the test).
	 */
	double ShearOnlyUnsqueezedUtilisation(
		double ForceMagnitudeUu, double AreaSqCm, const FConnectionStrength& Strength)
	{
		const double StressMPa = ForceMagnitudeUu / (AreaSqCm * InspectionForceUnitsPerMPaSqCm);
		return StressMPa / Strength.ShearCohesionMPa;
	}

	const TCHAR* NameOfRole(EJointRole Role)
	{
		switch (Role)
		{
		case EJointRole::None:       return TEXT("None");
		case EJointRole::BedBeneath: return TEXT("BedBeneath");
		case EJointRole::BedAbove:   return TEXT("BedAbove");
		case EJointRole::Head:       return TEXT("Head");
		}

		return TEXT("<not a role>");
	}

	const TCHAR* NameOfPieceSupport(EPieceSupport State)
	{
		switch (State)
		{
		case EPieceSupport::Falling:   return TEXT("Falling");
		case EPieceSupport::Grounded:  return TEXT("Grounded");
		case EPieceSupport::Supported: return TEXT("Supported");
		case EPieceSupport::Stranded:  return TEXT("Stranded");
		}

		return TEXT("<not a state>");
	}

	/** Describe an inspection for failure messages. */
	FString DescribeInspection(const FPieceInspection& Inspection)
	{
		FString Line = FString::Printf(
			TEXT("{piece:%s handle:%d answer:%s support:%s joints:"),
			Inspection.bIsPiece ? TEXT("yes") : TEXT("no"),
			Inspection.PieceIndex,
			Inspection.bHasSupportAnswer ? TEXT("yes") : TEXT("no"),
			NameOfPieceSupport(Inspection.Support));

		if (Inspection.Joints.Num() == 0)
		{
			return Line + TEXT("<none>}");
		}

		for (int32 Index = 0; Index < Inspection.Joints.Num(); ++Index)
		{
			const FJointInspection& Joint = Inspection.Joints[Index];
			Line += FString::Printf(
				TEXT("%s[c%d->p%d %s Fz=%.6f My=%.6f u=%.9f %s pass=%d]"),
				Index == 0 ? TEXT("") : TEXT(" "),
				Joint.ConnectionIndex,
				Joint.OtherPieceIndex,
				NameOfRole(Joint.Role),
				Joint.ForceUu.Z,
				Joint.MomentUuCm.Y,
				Joint.Utilisation,
				Joint.bHasGiven ? TEXT("GIVEN") : TEXT("intact"),
				Joint.BreakPass);
		}

		return Line + TEXT("}");
	}

	/** One expected row of a piece's breakout. */
	struct FExpectedJoint
	{
		int32 ConnectionIndex = INDEX_NONE;
		int32 OtherPieceIndex = INDEX_NONE;
		EJointRole Role = EJointRole::None;
		double ForceZUu = 0.0;
		double Utilisation = 0.0;
		bool bHasGiven = false;
		int32 BreakPass = INDEX_NONE;

		/**
		 * Bending moment about Y, uu.cm. Asserted zero on every worked-fixture row: with no joint
		 * geometry there is no lever arm (MOMENTS_DESIGN.md). Y because loads are vertical and
		 * rectangles axis-aligned. The eccentric fixture at the end derives a non-zero one.
		 */
		double MomentYUuCm = 0.0;
	};

	struct FExpectedPiece
	{
		const TCHAR* Description = nullptr;
		int32 PieceIndex = INDEX_NONE;
		bool bIsPiece = false;
		EPieceSupport Support = EPieceSupport::Falling;
		TArray<FExpectedJoint> Joints;
	};

	/**
	 * Every joint touching a piece, by brute force. Includes given joints, which the solver's
	 * support lists drop, so a breakout reusing those lists would fail here.
	 */
	TArray<int32> EveryJointTouching(const FStructure& Structure, int32 PieceIndex)
	{
		TArray<int32> Found;

		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			const FConnection& Connection = Structure.GetConnection(Index);
			if (Connection.PieceA == PieceIndex || Connection.PieceB == PieceIndex)
			{
				Found.Add(Index);
			}
		}

		return Found;
	}

	/** No readout number may be NaN or infinite. */
	void CheckEveryNumberIsUsable(
		FAutomationTestBase& Test, const TCHAR* Description, const FPieceInspection& Inspection)
	{
		for (const FJointInspection& Joint : Inspection.Joints)
		{
			Test.TestTrue(
				FString::Printf(
					TEXT("%s: connection %d must report a finite force, it reports (%f, %f, %f)"),
					Description, Joint.ConnectionIndex,
					Joint.ForceUu.X, Joint.ForceUu.Y, Joint.ForceUu.Z),
				FMath::IsFinite(Joint.ForceUu.X)
					&& FMath::IsFinite(Joint.ForceUu.Y)
					&& FMath::IsFinite(Joint.ForceUu.Z));

			// A moment has no Max() sentinel; a NaN would pass ComputeUtilisation's `!= 0.0` check.
			Test.TestTrue(
				FString::Printf(
					TEXT("%s: connection %d must report a finite moment, it reports (%f, %f, %f)"),
					Description, Joint.ConnectionIndex,
					Joint.MomentUuCm.X, Joint.MomentUuCm.Y, Joint.MomentUuCm.Z),
				FMath::IsFinite(Joint.MomentUuCm.X)
					&& FMath::IsFinite(Joint.MomentUuCm.Y)
					&& FMath::IsFinite(Joint.MomentUuCm.Z));

			/*
			 * Finite or exactly Max (the fail-closed answer). Never NaN: FMath::Max would discard it
			 * and silently report another axis.
			 */
			Test.TestTrue(
				FString::Printf(
					TEXT("%s: connection %d must report a usable utilisation, it reports %f"),
					Description, Joint.ConnectionIndex, Joint.Utilisation),
				!FMath::IsNaN(Joint.Utilisation)
					&& (FMath::IsFinite(Joint.Utilisation)
						|| Joint.Utilisation == TNumericLimits<double>::Max()));
		}
	}

	/**
	 * Every number on a row must equal its FStructure accessor exactly. A tolerance would admit a
	 * re-derivation that drifts in the last bit, which has happened twice.
	 */
	void CheckInspectionAgreesWithTheGraph(
		FAutomationTestBase& Test,
		const TCHAR* Description,
		const FStructure& Structure,
		const FPieceInspection& Inspection)
	{
		if (!Inspection.bIsPiece)
		{
			return;
		}

		const int32 Handle = Inspection.PieceIndex;

		Test.TestTrue(
			FString::Printf(
				TEXT("%s: support must be FStructure's own answer (%s), it reports %s"),
				Description,
				NameOfPieceSupport(Structure.GetPieceSupport(Handle)),
				NameOfPieceSupport(Inspection.Support)),
			Inspection.Support == Structure.GetPieceSupport(Handle));

		Test.TestTrue(
			FString::Printf(
				TEXT("%s: the have-we-solved flag must be HasSupportAnswer (%s), it reports %s"),
				Description,
				Structure.HasSupportAnswer(Handle) ? TEXT("yes") : TEXT("no"),
				Inspection.bHasSupportAnswer ? TEXT("yes") : TEXT("no")),
			Inspection.bHasSupportAnswer == Structure.HasSupportAnswer(Handle));

		for (const FJointInspection& Joint : Inspection.Joints)
		{
			const int32 Index = Joint.ConnectionIndex;

			Test.TestTrue(
				FString::Printf(
					TEXT("%s: connection %d's force must be GetConnectionForce EXACTLY; ")
					TEXT("the graph says %.17g and the breakout says %.17g"),
					Description, Index,
					Structure.GetConnectionForce(Index).Z, Joint.ForceUu.Z),
				Joint.ForceUu == Structure.GetConnectionForce(Index));

			// The moment too: UtilisationUnder defaults it, so a breakout that skipped it would read zero.
			Test.TestTrue(
				FString::Printf(
					TEXT("%s: connection %d's moment must be GetConnectionMoment EXACTLY; ")
					TEXT("the graph says (%.17g, %.17g, %.17g) and the breakout says (%.17g, %.17g, %.17g)"),
					Description, Index,
					Structure.GetConnectionMoment(Index).X,
					Structure.GetConnectionMoment(Index).Y,
					Structure.GetConnectionMoment(Index).Z,
					Joint.MomentUuCm.X, Joint.MomentUuCm.Y, Joint.MomentUuCm.Z),
				Joint.MomentUuCm == Structure.GetConnectionMoment(Index));

			Test.TestTrue(
				FString::Printf(
					TEXT("%s: connection %d's utilisation must be GetConnectionUtilisation ")
					TEXT("EXACTLY; the graph says %.17g and the breakout says %.17g"),
					Description, Index,
					Structure.GetConnectionUtilisation(Index), Joint.Utilisation),
				Joint.Utilisation == Structure.GetConnectionUtilisation(Index));

			Test.TestTrue(
				FString::Printf(
					TEXT("%s: connection %d's role must be GetJointRole for piece %d (%s), ")
					TEXT("the breakout says %s"),
					Description, Index, Handle,
					NameOfRole(Structure.GetJointRole(Index, Handle)),
					NameOfRole(Joint.Role)),
				Joint.Role == Structure.GetJointRole(Index, Handle));

			Test.TestTrue(
				FString::Printf(
					TEXT("%s: connection %d's given state must be FConnection's own latch (%s), ")
					TEXT("the breakout says %s"),
					Description, Index,
					Structure.GetConnection(Index).HasGiven() ? TEXT("given") : TEXT("intact"),
					Joint.bHasGiven ? TEXT("given") : TEXT("intact")),
				Joint.bHasGiven == Structure.GetConnection(Index).HasGiven());

			Test.TestTrue(
				FString::Printf(
					TEXT("%s: connection %d's break pass must be GetBreakPass (%d), ")
					TEXT("the breakout says %d"),
					Description, Index, Structure.GetBreakPass(Index), Joint.BreakPass),
				Joint.BreakPass == Structure.GetBreakPass(Index));
		}
	}

	/**
	 * The role rule (DESIGN.md §3) stated as an angle from vertical of the normal toward the piece:
	 * under 45 bears, over 135 rests on it, between is a head joint. Deliberately not production's
	 * cosine form, so agreement is evidence. The other end of the joint is 180 minus this.
	 */
	EJointRole RoleFromAngleOfNormalTowardPiece(double AngleFromVerticalDeg)
	{
		if (AngleFromVerticalDeg < 45.0)
		{
			return EJointRole::BedBeneath;
		}

		if (AngleFromVerticalDeg > 135.0)
		{
			return EJointRole::BedAbove;
		}

		return EJointRole::Head;
	}

	/** One spoke of the star graph in the role test. */
	struct FRoleCase
	{
		const TCHAR* Description = nullptr;

		/** Angle of the interface normal from straight up, in degrees. */
		double AngleDeg = 0.0;

		/**
		 * Whether the expected role is asserted. False only at exactly 45/135 degrees, where the
		 * normal lands an ulp either side of cos 45; those rows assert only non-None and agreement.
		 */
		bool bRoleIsWellDefined = true;
	};
}

/**
 * A piece's breakout lists every joint touching it, including given ones, each with neighbour,
 * role, force, moment, utilisation and given state, plus the piece's support state. Every number
 * is the solver's own answer read back, never re-derived.
 *
 * The moment is on the row because utilisation depends on it; without it a near-limit joint
 * cannot be explained. The eccentric fixture at the end is the only one that bends.
 *
 * Two kinds of assertion, both needed: a hand-derived table (catches a wrong chain, e.g. a 100x
 * slip) and an exact agreement sweep against FStructure (catches a second copy of the solver).
 * Each bed joint is loaded only in compression and the head joint only in shear, so the governing
 * axis is known. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceInspectionJointBreakoutTest,
	"DestructionGame.Core.PieceInspection.JointBreakout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceInspectionJointBreakoutTest::RunTest(const FString& Parameters)
{
	using namespace PieceInspectionTestSupport;

	// Fixture precondition: with no compression, head-joint capacity is min(cohesion, cap).
	TestTrue(
		FString::Printf(
			TEXT("fixture: mortar's shear truncation cap (%.3f MPa) must sit above its cohesion ")
			TEXT("(%.3f MPa), or an unsqueezed head joint stops being a cohesion test"),
			GeneralPurposeMortar.MaxShearStrengthMPa, GeneralPurposeMortar.ShearCohesionMPa),
		GeneralPurposeMortar.MaxShearStrengthMPa > GeneralPurposeMortar.ShearCohesionMPa);

	FStructureBinding Binding;
	BuildWorkedFixture(Binding, /*bPullThePad*/ true, /*bSolve*/ true);

	const FStructure& Structure = Binding.GetStructure();

	/*
	 * Hand-derived loads. The hanger hangs its weight on the head joint; the rider rests on the
	 * subject; the one remaining bed joint under the subject carries 2 + 3 + 1 = 6 kg.
	 */
	const double HangerLoadUu = HangerMassKg * InspectionGravityCmPerSecondSquared;
	const double RiderLoadUu = RiderMassKg * InspectionGravityCmPerSecondSquared;
	const double PadLoadUu =
		(SubjectMassKg + HangerMassKg + RiderMassKg) * InspectionGravityCmPerSecondSquared;

	/*
	 * Force acts on PieceB (ConnectionLoad.h) and each joint names the loaded piece second, so
	 * every force points down.
	 *
	 * Fixture preconditions, read from the graph directly, so the hand-derived numbers are checked
	 * even if the breakout returns no rows. Green on arrival by design.
	 */
	TestTrue(
		FString::Printf(
			TEXT("fixture: the pad joint should carry %.6f uu at %.12f utilisation; ")
			TEXT("the graph says %.6f uu at %.12f"),
			-PadLoadUu,
			CompressionOnlyUtilisation(PadLoadUu, InspectionJointAreaSqCm, GeneralPurposeMortar),
			Structure.GetConnectionForce(PadJoint).Z,
			Structure.GetConnectionUtilisation(PadJoint)),
		FMath::IsNearlyEqual(Structure.GetConnectionForce(PadJoint).Z, -PadLoadUu, 1.0e-9)
			&& FMath::IsNearlyEqual(
				Structure.GetConnectionUtilisation(PadJoint),
				CompressionOnlyUtilisation(PadLoadUu, InspectionJointAreaSqCm, GeneralPurposeMortar),
				1.0e-12));

	TestTrue(
		FString::Printf(
			TEXT("fixture: the head joint should carry %.6f uu in SHEAR at %.12f utilisation; ")
			TEXT("the graph says %.6f uu at %.12f"),
			-HangerLoadUu,
			ShearOnlyUnsqueezedUtilisation(HangerLoadUu, InspectionJointAreaSqCm, GeneralPurposeMortar),
			Structure.GetConnectionForce(HangerJoint).Z,
			Structure.GetConnectionUtilisation(HangerJoint)),
		FMath::IsNearlyEqual(Structure.GetConnectionForce(HangerJoint).Z, -HangerLoadUu, 1.0e-9)
			&& FMath::IsNearlyEqual(
				Structure.GetConnectionUtilisation(HangerJoint),
				ShearOnlyUnsqueezedUtilisation(HangerLoadUu, InspectionJointAreaSqCm, GeneralPurposeMortar),
				1.0e-12));

	TestTrue(
		FString::Printf(
			TEXT("fixture: the rider joint should carry %.6f uu at %.12f utilisation; ")
			TEXT("the graph says %.6f uu at %.12f"),
			-RiderLoadUu,
			CompressionOnlyUtilisation(RiderLoadUu, InspectionJointAreaSqCm, GeneralPurposeMortar),
			Structure.GetConnectionForce(RiderJoint).Z,
			Structure.GetConnectionUtilisation(RiderJoint)),
		FMath::IsNearlyEqual(Structure.GetConnectionForce(RiderJoint).Z, -RiderLoadUu, 1.0e-9)
			&& FMath::IsNearlyEqual(
				Structure.GetConnectionUtilisation(RiderJoint),
				CompressionOnlyUtilisation(RiderLoadUu, InspectionJointAreaSqCm, GeneralPurposeMortar),
				1.0e-12));

	// The severed joint reads like an intact unloaded one; only bHasGiven tells them apart.
	TestTrue(
		FString::Printf(
			TEXT("fixture: the severed joint should carry nothing at zero utilisation, ")
			TEXT("and report itself given with no break pass; the graph says %.6f uu, %.12f, %s, pass %d"),
			Structure.GetConnectionForce(PulledJoint).Z,
			Structure.GetConnectionUtilisation(PulledJoint),
			Structure.GetConnection(PulledJoint).HasGiven() ? TEXT("given") : TEXT("intact"),
			Structure.GetBreakPass(PulledJoint)),
		FMath::IsNearlyEqual(Structure.GetConnectionForce(PulledJoint).Z, 0.0, 1.0e-9)
			&& FMath::IsNearlyEqual(Structure.GetConnectionUtilisation(PulledJoint), 0.0, 1.0e-12)
			&& Structure.GetConnection(PulledJoint).HasGiven()
			&& Structure.GetBreakPass(PulledJoint) == INDEX_NONE);

	TestTrue(
		FString::Printf(
			TEXT("fixture: the four pieces left should read Grounded/Supported/Supported/Supported ")
			TEXT("and the pulled pad should be gone; they read %s/%s/%s/%s, removed=%s"),
			NameOfPieceSupport(Structure.GetPieceSupport(PadPiece)),
			NameOfPieceSupport(Structure.GetPieceSupport(SubjectPiece)),
			NameOfPieceSupport(Structure.GetPieceSupport(HangerPiece)),
			NameOfPieceSupport(Structure.GetPieceSupport(RiderPiece)),
			Structure.IsPieceRemoved(PulledPadPiece) ? TEXT("yes") : TEXT("no")),
		Structure.GetPieceSupport(PadPiece) == EPieceSupport::Grounded
			&& Structure.GetPieceSupport(SubjectPiece) == EPieceSupport::Supported
			&& Structure.GetPieceSupport(HangerPiece) == EPieceSupport::Supported
			&& Structure.GetPieceSupport(RiderPiece) == EPieceSupport::Supported
			&& Structure.IsPieceRemoved(PulledPadPiece));

	const TArray<FExpectedPiece> Cases = {
		{
			TEXT("the grounded pad, which the whole fixture rests on"),
			PadPiece,
			true,
			EPieceSupport::Grounded,
			{
				{
					PadJoint, SubjectPiece, EJointRole::BedAbove,
					-PadLoadUu,
					CompressionOnlyUtilisation(PadLoadUu, InspectionJointAreaSqCm, GeneralPurposeMortar),
					false, INDEX_NONE
				},
			}
		},
		{
			// All three roles plus a severed joint; a supports-only breakout would show one row.
			TEXT("the subject, which has all three roles and one severed joint"),
			SubjectPiece,
			true,
			EPieceSupport::Supported,
			{
				{
					PadJoint, PadPiece, EJointRole::BedBeneath,
					-PadLoadUu,
					CompressionOnlyUtilisation(PadLoadUu, InspectionJointAreaSqCm, GeneralPurposeMortar),
					false, INDEX_NONE
				},
				{
					HangerJoint, HangerPiece, EJointRole::Head,
					-HangerLoadUu,
					ShearOnlyUnsqueezedUtilisation(HangerLoadUu, InspectionJointAreaSqCm, GeneralPurposeMortar),
					false, INDEX_NONE
				},
				{
					RiderJoint, RiderPiece, EJointRole::BedAbove,
					-RiderLoadUu,
					CompressionOnlyUtilisation(RiderLoadUu, InspectionJointAreaSqCm, GeneralPurposeMortar),
					false, INDEX_NONE
				},
				{
					/*
					 * Went with a removed piece: given but unstamped (not a failure, DESIGN.md).
					 * Still names piece 4 and its former role.
					 */
					PulledJoint, PulledPadPiece, EJointRole::BedBeneath,
					0.0, 0.0, true, INDEX_NONE
				},
			}
		},
		{
			TEXT("the hanger, held only by a head joint"),
			HangerPiece,
			true,
			EPieceSupport::Supported,
			{
				{
					HangerJoint, SubjectPiece, EJointRole::Head,
					-HangerLoadUu,
					ShearOnlyUnsqueezedUtilisation(HangerLoadUu, InspectionJointAreaSqCm, GeneralPurposeMortar),
					false, INDEX_NONE
				},
			}
		},
		{
			TEXT("the rider, resting on the subject"),
			RiderPiece,
			true,
			EPieceSupport::Supported,
			{
				{
					RiderJoint, SubjectPiece, EJointRole::BedBeneath,
					-RiderLoadUu,
					CompressionOnlyUtilisation(RiderLoadUu, InspectionJointAreaSqCm, GeneralPurposeMortar),
					false, INDEX_NONE
				},
			}
		},
		{
			// A removed piece gets no joints and no support state, not even the stale one.
			TEXT("the pad the player pulled out"),
			PulledPadPiece,
			false,
			EPieceSupport::Falling,
			{}
		},
	};

	for (const FExpectedPiece& Case : Cases)
	{
		const FPieceInspection Inspection = InspectPiece(Structure, Case.PieceIndex);

		TestTrue(
			FString::Printf(TEXT("%s should%s be a piece; got %s"),
				Case.Description, Case.bIsPiece ? TEXT("") : TEXT(" not"),
				*DescribeInspection(Inspection)),
			Inspection.bIsPiece == Case.bIsPiece);

		TestTrue(
			FString::Printf(TEXT("%s should report support %s; got %s"),
				Case.Description, NameOfPieceSupport(Case.Support),
				*DescribeInspection(Inspection)),
			Inspection.Support == Case.Support);

		TestEqual(
			FString::Printf(TEXT("%s should break out %d joint(s); got %s"),
				Case.Description, Case.Joints.Num(), *DescribeInspection(Inspection)),
			Inspection.Joints.Num(), Case.Joints.Num());

		CheckEveryNumberIsUsable(*this, Case.Description, Inspection);
		CheckInspectionAgreesWithTheGraph(*this, Case.Description, Structure, Inspection);

		if (Inspection.Joints.Num() != Case.Joints.Num())
		{
			continue;
		}

		for (int32 Row = 0; Row < Case.Joints.Num(); ++Row)
		{
			const FJointInspection& Actual = Inspection.Joints[Row];
			const FExpectedJoint& Expected = Case.Joints[Row];

			// Row order is part of the contract.
			TestEqual(
				FString::Printf(TEXT("%s: row %d should be connection %d; got %s"),
					Case.Description, Row, Expected.ConnectionIndex, *DescribeInspection(Inspection)),
				Actual.ConnectionIndex, Expected.ConnectionIndex);

			TestEqual(
				FString::Printf(TEXT("%s: row %d should reach piece %d; got %s"),
					Case.Description, Row, Expected.OtherPieceIndex, *DescribeInspection(Inspection)),
				Actual.OtherPieceIndex, Expected.OtherPieceIndex);

			TestTrue(
				FString::Printf(TEXT("%s: row %d should be a %s joint to this piece; got %s"),
					Case.Description, Row, NameOfRole(Expected.Role), *DescribeInspection(Inspection)),
				Actual.Role == Expected.Role);

			// Unreal force units, unconverted; a newton conversion here would be out by 100x.
			TestTrue(
				FString::Printf(TEXT("%s: row %d should carry %.6f uu downward; got %.6f uu"),
					Case.Description, Row, Expected.ForceZUu, Actual.ForceUu.Z),
				FMath::IsNearlyEqual(Actual.ForceUu.Z, Expected.ForceZUu, 1.0e-9));

			TestTrue(
				FString::Printf(
					TEXT("%s: row %d should carry nothing sideways; got (%.6f, %.6f)"),
					Case.Description, Row, Actual.ForceUu.X, Actual.ForceUu.Y),
				FMath::IsNearlyEqual(Actual.ForceUu.X, 0.0, 1.0e-9)
					&& FMath::IsNearlyEqual(Actual.ForceUu.Y, 0.0, 1.0e-9));

			// No moment on any axis in a geometry-free fixture.
			TestTrue(
				FString::Printf(
					TEXT("%s: row %d should carry no moment at all; got (%.6f, %.6f, %.6f)"),
					Case.Description, Row,
					Actual.MomentUuCm.X, Actual.MomentUuCm.Y, Actual.MomentUuCm.Z),
				FMath::IsNearlyEqual(Actual.MomentUuCm.X, 0.0, 1.0e-9)
					&& FMath::IsNearlyEqual(Actual.MomentUuCm.Y, Expected.MomentYUuCm, 1.0e-9)
					&& FMath::IsNearlyEqual(Actual.MomentUuCm.Z, 0.0, 1.0e-9));

			TestTrue(
				FString::Printf(TEXT("%s: row %d should read %.12f utilisation; got %.12f"),
					Case.Description, Row, Expected.Utilisation, Actual.Utilisation),
				FMath::IsNearlyEqual(Actual.Utilisation, Expected.Utilisation, 1.0e-12));

			TestTrue(
				FString::Printf(TEXT("%s: row %d should read %s; got %s"),
					Case.Description, Row,
					Expected.bHasGiven ? TEXT("GIVEN") : TEXT("intact"),
					*DescribeInspection(Inspection)),
				Actual.bHasGiven == Expected.bHasGiven);

			TestEqual(
				FString::Printf(TEXT("%s: row %d should carry break pass %d; got %s"),
					Case.Description, Row, Expected.BreakPass, *DescribeInspection(Inspection)),
				Actual.BreakPass, Expected.BreakPass);
		}
	}

	// Completeness: every live piece breaks out exactly the joints touching it, in ascending order.
	for (int32 Handle = 0; Handle < Structure.NumPieces(); ++Handle)
	{
		if (Structure.IsPieceRemoved(Handle))
		{
			continue;
		}

		const FPieceInspection Inspection = InspectPiece(Structure, Handle);
		const TArray<int32> Expected = EveryJointTouching(Structure, Handle);

		TArray<int32> Reported;
		for (const FJointInspection& Joint : Inspection.Joints)
		{
			Reported.Add(Joint.ConnectionIndex);
		}

		TestTrue(
			FString::Printf(
				TEXT("piece %d should break out exactly the joints touching it [%s]; it broke out [%s]"),
				Handle,
				*FString::JoinBy(Expected, TEXT(","), [](int32 I) { return FString::FromInt(I); }),
				*FString::JoinBy(Reported, TEXT(","), [](int32 I) { return FString::FromInt(I); })),
			Reported == Expected);
	}

	// The ref overload must return the whole same answer as the handle overload.
	for (int32 Handle = 0; Handle < Structure.NumPieces(); ++Handle)
	{
		const FPieceInspection ByHandle = InspectPiece(Structure, Handle);
		const FPieceInspection ByRef = InspectPiece(Binding, MakeRef(ThisStructure, Handle));

		bool bSame = ByRef.bIsPiece == ByHandle.bIsPiece
			&& ByRef.PieceIndex == ByHandle.PieceIndex
			&& ByRef.bHasSupportAnswer == ByHandle.bHasSupportAnswer
			&& ByRef.Support == ByHandle.Support
			&& ByRef.Joints.Num() == ByHandle.Joints.Num();

		for (int32 Row = 0; bSame && Row < ByHandle.Joints.Num(); ++Row)
		{
			bSame = ByRef.Joints[Row].ConnectionIndex == ByHandle.Joints[Row].ConnectionIndex
				&& ByRef.Joints[Row].OtherPieceIndex == ByHandle.Joints[Row].OtherPieceIndex
				&& ByRef.Joints[Row].Role == ByHandle.Joints[Row].Role
				&& ByRef.Joints[Row].ForceUu == ByHandle.Joints[Row].ForceUu
				&& ByRef.Joints[Row].MomentUuCm == ByHandle.Joints[Row].MomentUuCm
				&& ByRef.Joints[Row].Utilisation == ByHandle.Joints[Row].Utilisation
				&& ByRef.Joints[Row].bHasGiven == ByHandle.Joints[Row].bHasGiven
				&& ByRef.Joints[Row].BreakPass == ByHandle.Joints[Row].BreakPass;
		}

		TestTrue(
			FString::Printf(
				TEXT("inspecting piece %d by ref must give the same answer as by handle; ")
				TEXT("by handle %s, by ref %s"),
				Handle, *DescribeInspection(ByHandle), *DescribeInspection(ByRef)),
			bSame);
	}

	/*
	 * A joint that failed under load, stamped with its break pass. Its own two-piece structure
	 * because it needs SolveAndBreak, which FStructureBinding does not expose.
	 */
	FStructure Cascaded;
	const int32 CascadePad = Cascaded.AddPiece(1.0, /*bIsGrounded*/ true);
	const int32 CascadeLoad = Cascaded.AddPiece(200.0);

	FConnection Overloaded;
	Overloaded.PieceA = CascadePad;
	Overloaded.PieceB = CascadeLoad;
	Overloaded.InterfaceNormal = InspectionBedNormal;

	// 200 kg on 1 cm2: 196,000 uu = 19.6 MPa against 10 MPa, so it gives at 1.96 in pass 1.
	Overloaded.InterfaceAreaSqCm = 1.0;
	Overloaded.Strength = GeneralPurposeMortar;
	const int32 OverloadedJoint = Cascaded.AddConnection(Overloaded);

	const int32 Passes = Cascaded.SolveAndBreak();

	TestEqual(
		FString::Printf(
			TEXT("fixture: the overloaded joint must give in exactly one pass; the cascade ran %d"),
			Passes),
		Passes, 1);

	const FPieceInspection Broken = InspectPiece(Cascaded, CascadeLoad);

	CheckEveryNumberIsUsable(*this, TEXT("the piece whose joint failed"), Broken);
	CheckInspectionAgreesWithTheGraph(*this, TEXT("the piece whose joint failed"), Cascaded, Broken);

	TestEqual(
		FString::Printf(TEXT("the piece whose joint failed should still break out its one joint; got %s"),
			*DescribeInspection(Broken)),
		Broken.Joints.Num(), 1);

	if (Broken.Joints.Num() == 1)
	{
		// The break pass is the only field distinguishing a failed joint from a severed one.
		TestEqual(
			FString::Printf(TEXT("the failed joint should be stamped with pass 1; got %s"),
				*DescribeInspection(Broken)),
			Broken.Joints[0].BreakPass, 1);

		TestTrue(
			FString::Printf(TEXT("the failed joint should read as given; got %s"),
				*DescribeInspection(Broken)),
			Broken.Joints[0].bHasGiven);

		TestTrue(
			FString::Printf(TEXT("the failed joint should now carry nothing; got %s"),
				*DescribeInspection(Broken)),
			FMath::IsNearlyEqual(Broken.Joints[0].ForceUu.Z, 0.0, 1.0e-9)
				&& FMath::IsNearlyEqual(Broken.Joints[0].Utilisation, 0.0, 1.0e-12));

		TestTrue(
			FString::Printf(TEXT("the failed joint should still report the tier it was; got %s"),
				*DescribeInspection(Broken)),
			Broken.Joints[0].Role == EJointRole::BedBeneath);
	}

	TestTrue(
		FString::Printf(TEXT("the piece whose joint failed should now read Falling; got %s"),
			*DescribeInspection(Broken)),
		Broken.Support == EPieceSupport::Falling);

	TestEqual(
		FString::Printf(TEXT("connection %d must be the one that broke"), OverloadedJoint),
		Cascaded.GetBreakPass(OverloadedJoint), 1);

	/*
	 * An eccentric joint, the only fixture here that bends; without it a breakout that never read
	 * the moment would pass every row. Its own structure so the worked table's numbers stay put.
	 *
	 *   force        8 kg x 980 = 7,840 uu down
	 *   moment       lever arm 4 cm on X, so 4 x 7,840 = 31,360 uu.cm about Y
	 *   section      6 x 8 cm: area 48 cm2, modulus b.d2/6 = 8 x 36 / 6 = 48 cm3 (textbook form,
	 *                not production's, so agreement is evidence)
	 *   stresses     mean 0.0163333 MPa, edge 0.0653333 MPa
	 *   peak tension 0.049 MPa against mortar's 0.7, so 0.07 (expectation divides by the profile)
	 *
	 * Tension governs: peak compression is 0.0082 of capacity and shear is zero.
	 */
	constexpr double EccentricMassKg = 8.0;
	constexpr double EccentricLeverArmCm = 4.0;
	constexpr double EccentricHalfXCm = 3.0;
	constexpr double EccentricHalfYCm = 4.0;

	const double EccentricAreaSqCm = 4.0 * EccentricHalfXCm * EccentricHalfYCm;

	// b.d2/6 with depth along the lean (X).
	const double EccentricModulusCm3 =
		(2.0 * EccentricHalfYCm) * FMath::Square(2.0 * EccentricHalfXCm) / 6.0;

	const double EccentricForceUu = EccentricMassKg * InspectionGravityCmPerSecondSquared;
	const double EccentricMomentYUuCm = EccentricLeverArmCm * EccentricForceUu;

	const double EccentricMeanStressMPa =
		EccentricForceUu / (EccentricAreaSqCm * InspectionForceUnitsPerMPaSqCm);

	const double EccentricEdgeStressMPa =
		EccentricMomentYUuCm / (EccentricModulusCm3 * InspectionForceUnitsPerMPaSqCm);

	const double EccentricUtilisation =
		(EccentricEdgeStressMPa - EccentricMeanStressMPa) / GeneralPurposeMortar.TensileStrengthMPa;

	FStructure Eccentric;

	const int32 EccentricPad =
		Eccentric.AddPiece(50.0, /*bIsGrounded*/ true, FVector(0.0, 0.0, 0.0));

	// 4 cm along X from the joint below it.
	const int32 Overhang = Eccentric.AddPiece(
		EccentricMassKg, /*bIsGrounded*/ false,
		FVector(EccentricLeverArmCm, 0.0, 10.0));

	FConnection EccentricBed;
	EccentricBed.PieceA = EccentricPad;
	EccentricBed.PieceB = Overhang;
	EccentricBed.InterfaceNormal = InspectionBedNormal;
	EccentricBed.InterfaceAreaSqCm = EccentricAreaSqCm;
	EccentricBed.InterfaceCentreCm = FVector(0.0, 0.0, 5.0);
	EccentricBed.InterfaceHalfExtentCm =
		FVector(EccentricHalfXCm, EccentricHalfYCm, 0.0);
	EccentricBed.Strength = GeneralPurposeMortar;

	const int32 EccentricJoint = Eccentric.AddConnection(EccentricBed);

	// AddConnection returns INDEX_NONE for inconsistent geometry; then there is no joint to test.
	TestTrue(
		FString::Printf(
			TEXT("fixture: the eccentric bed joint must be accepted — %.1f cm2 against a %.1f x %.1f half-rectangle; AddConnection returned %d"),
			EccentricAreaSqCm, EccentricHalfXCm, EccentricHalfYCm, EccentricJoint),
		EccentricJoint != INDEX_NONE);

	if (EccentricJoint == INDEX_NONE)
	{
		return true;
	}

	Eccentric.SolveLoads();

	// Fixture preconditions from the graph directly, so the fixture is proven to bend.
	TestTrue(
		FString::Printf(
			TEXT("fixture: the eccentric joint should carry %.6f uu straight down; the graph says (%.6f, %.6f, %.6f)"),
			-EccentricForceUu,
			Eccentric.GetConnectionForce(EccentricJoint).X,
			Eccentric.GetConnectionForce(EccentricJoint).Y,
			Eccentric.GetConnectionForce(EccentricJoint).Z),
		FMath::IsNearlyEqual(Eccentric.GetConnectionForce(EccentricJoint).Z, -EccentricForceUu, 1.0e-9)
			&& FMath::IsNearlyEqual(Eccentric.GetConnectionForce(EccentricJoint).X, 0.0, 1.0e-9)
			&& FMath::IsNearlyEqual(Eccentric.GetConnectionForce(EccentricJoint).Y, 0.0, 1.0e-9));

	TestTrue(
		FString::Printf(
			TEXT("fixture: the eccentric joint should bend by %.6f uu.cm about Y; the graph says (%.6f, %.6f, %.6f)"),
			EccentricMomentYUuCm,
			Eccentric.GetConnectionMoment(EccentricJoint).X,
			Eccentric.GetConnectionMoment(EccentricJoint).Y,
			Eccentric.GetConnectionMoment(EccentricJoint).Z),
		FMath::IsNearlyEqual(Eccentric.GetConnectionMoment(EccentricJoint).Y, EccentricMomentYUuCm, 1.0e-9)
			&& FMath::IsNearlyEqual(Eccentric.GetConnectionMoment(EccentricJoint).X, 0.0, 1.0e-9)
			&& FMath::IsNearlyEqual(Eccentric.GetConnectionMoment(EccentricJoint).Z, 0.0, 1.0e-9));

	TestTrue(
		FString::Printf(
			TEXT("fixture: the eccentric joint should sit at %.12f of capacity in TENSION; the graph says %.12f"),
			EccentricUtilisation, Eccentric.GetConnectionUtilisation(EccentricJoint)),
		FMath::IsNearlyEqual(
			Eccentric.GetConnectionUtilisation(EccentricJoint), EccentricUtilisation, 1.0e-12));

	// Both ends are inspected: the moment belongs to the joint, so both must read the same.
	const FPieceInspection LeaningBrick = InspectPiece(Eccentric, Overhang);
	const FPieceInspection PadBeneathIt = InspectPiece(Eccentric, EccentricPad);

	CheckEveryNumberIsUsable(*this, TEXT("the brick leaning off its joint"), LeaningBrick);
	CheckInspectionAgreesWithTheGraph(
		*this, TEXT("the brick leaning off its joint"), Eccentric, LeaningBrick);

	CheckEveryNumberIsUsable(*this, TEXT("the pad the leaning brick stands on"), PadBeneathIt);
	CheckInspectionAgreesWithTheGraph(
		*this, TEXT("the pad the leaning brick stands on"), Eccentric, PadBeneathIt);

	TestEqual(
		FString::Printf(TEXT("the leaning brick should break out its one joint; got %s"),
			*DescribeInspection(LeaningBrick)),
		LeaningBrick.Joints.Num(), 1);

	TestEqual(
		FString::Printf(TEXT("the pad should break out the same one joint; got %s"),
			*DescribeInspection(PadBeneathIt)),
		PadBeneathIt.Joints.Num(), 1);

	if (LeaningBrick.Joints.Num() == 1 && PadBeneathIt.Joints.Num() == 1)
	{
		TestTrue(
			FString::Printf(
				TEXT("the leaning brick's row should carry a moment of (0, %.6f, 0) uu.cm; got %s"),
				EccentricMomentYUuCm, *DescribeInspection(LeaningBrick)),
			FMath::IsNearlyEqual(LeaningBrick.Joints[0].MomentUuCm.Y, EccentricMomentYUuCm, 1.0e-9)
				&& FMath::IsNearlyEqual(LeaningBrick.Joints[0].MomentUuCm.X, 0.0, 1.0e-9)
				&& FMath::IsNearlyEqual(LeaningBrick.Joints[0].MomentUuCm.Z, 0.0, 1.0e-9));

		TestTrue(
			FString::Printf(
				TEXT("the pad's row should carry the SAME moment as the brick's — one joint, one bend; got %s and %s"),
				*DescribeInspection(PadBeneathIt), *DescribeInspection(LeaningBrick)),
			PadBeneathIt.Joints[0].MomentUuCm == LeaningBrick.Joints[0].MomentUuCm);

		// The force is unchanged by the bend (MOMENTS_DESIGN.md keeps them separate).
		TestTrue(
			FString::Printf(
				TEXT("the leaning brick's row should still carry a plain vertical %.6f uu; got %s"),
				-EccentricForceUu, *DescribeInspection(LeaningBrick)),
			FMath::IsNearlyEqual(LeaningBrick.Joints[0].ForceUu.Z, -EccentricForceUu, 1.0e-9)
				&& FMath::IsNearlyEqual(LeaningBrick.Joints[0].ForceUu.X, 0.0, 1.0e-9)
				&& FMath::IsNearlyEqual(LeaningBrick.Joints[0].ForceUu.Y, 0.0, 1.0e-9));
	}

	return true;
}

/**
 * A breakout row's role is the solver's bed/head decision for the inspected piece, and flips when
 * the joint is seen from the other end. The expectation comes from the angle, not production's
 * cosine. A star: one centre, one spoke per angle. Unsolved on purpose, since a role is pure
 * geometry. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceInspectionRoleTest,
	"DestructionGame.Core.PieceInspection.RoleIsTheSolversTwoTierDecision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceInspectionRoleTest::RunTest(const FString& Parameters)
{
	using namespace PieceInspectionTestSupport;

	const TArray<FRoleCase> Cases = {
		{ TEXT("straight up"), 0.0 },
		{ TEXT("a shallow tilt"), 15.0 },
		{ TEXT("just inside the bearing line"), 44.0 },
		{ TEXT("exactly on the 45 degree line"), 45.0, /*bRoleIsWellDefined*/ false },
		{ TEXT("just outside the bearing line"), 46.0 },
		{ TEXT("dead horizontal"), 90.0 },
		{ TEXT("just above the mirror line"), 134.0 },
		{ TEXT("exactly on the 135 degree line"), 135.0, /*bRoleIsWellDefined*/ false },
		{ TEXT("just past the mirror line"), 136.0 },
		{ TEXT("nearly straight down"), 165.0 },
		{ TEXT("straight down"), 180.0 },
	};

	FStructure Structure;
	const int32 Centre = Structure.AddPiece(1.0, /*bIsGrounded*/ true);

	TArray<int32> Spokes;

	for (const FRoleCase& Case : Cases)
	{
		const double Radians = FMath::DegreesToRadians(Case.AngleDeg);

		FConnection Connection;
		Connection.PieceA = Centre;
		Connection.PieceB = Structure.AddPiece(1.0);
		Connection.InterfaceNormal = FVector(FMath::Sin(Radians), 0.0, FMath::Cos(Radians));
		Connection.InterfaceAreaSqCm = InspectionJointAreaSqCm;
		Connection.Strength = GeneralPurposeMortar;

		Spokes.Add(Connection.PieceB);
		Structure.AddConnection(Connection);
	}

	const FPieceInspection CentreInspection = InspectPiece(Structure, Centre);

	CheckEveryNumberIsUsable(*this, TEXT("the centre of the star"), CentreInspection);
	CheckInspectionAgreesWithTheGraph(*this, TEXT("the centre of the star"), Structure, CentreInspection);

	TestEqual(
		FString::Printf(TEXT("the centre should break out one joint per spoke; got %s"),
			*DescribeInspection(CentreInspection)),
		CentreInspection.Joints.Num(), Cases.Num());

	if (CentreInspection.Joints.Num() != Cases.Num())
	{
		return true;
	}

	for (int32 Index = 0; Index < Cases.Num(); ++Index)
	{
		const FRoleCase& Case = Cases[Index];

		// The normal points at the spoke; toward the centre the angle is the supplement.
		const EJointRole ExpectedForSpoke = RoleFromAngleOfNormalTowardPiece(Case.AngleDeg);
		const EJointRole ExpectedForCentre = RoleFromAngleOfNormalTowardPiece(180.0 - Case.AngleDeg);

		const FJointInspection& FromCentre = CentreInspection.Joints[Index];

		const FPieceInspection SpokeInspection = InspectPiece(Structure, Spokes[Index]);
		CheckEveryNumberIsUsable(*this, Case.Description, SpokeInspection);
		CheckInspectionAgreesWithTheGraph(*this, Case.Description, Structure, SpokeInspection);

		TestEqual(
			FString::Printf(TEXT("%s: the spoke should break out its one joint; got %s"),
				Case.Description, *DescribeInspection(SpokeInspection)),
			SpokeInspection.Joints.Num(), 1);

		if (SpokeInspection.Joints.Num() != 1)
		{
			continue;
		}

		const FJointInspection& FromSpoke = SpokeInspection.Joints[0];

		// Asserted on every row, boundary included: a listed joint is never None.
		TestTrue(
			FString::Printf(TEXT("%s (%.0f deg): the centre's row must carry a real role; got %s"),
				Case.Description, Case.AngleDeg, NameOfRole(FromCentre.Role)),
			FromCentre.Role != EJointRole::None);

		TestTrue(
			FString::Printf(TEXT("%s (%.0f deg): the spoke's row must carry a real role; got %s"),
				Case.Description, Case.AngleDeg, NameOfRole(FromSpoke.Role)),
			FromSpoke.Role != EJointRole::None);

		if (!Case.bRoleIsWellDefined)
		{
			continue;
		}

		TestTrue(
			FString::Printf(
				TEXT("%s (%.0f deg): to the spoke this joint should be %s; it reports %s"),
				Case.Description, Case.AngleDeg, NameOfRole(ExpectedForSpoke), NameOfRole(FromSpoke.Role)),
			FromSpoke.Role == ExpectedForSpoke);

		// The flip: the other end sees the mirror role (Head mirrors to Head).
		TestTrue(
			FString::Printf(
				TEXT("%s (%.0f deg): to the centre the same joint should be %s; it reports %s"),
				Case.Description, Case.AngleDeg, NameOfRole(ExpectedForCentre), NameOfRole(FromCentre.Role)),
			FromCentre.Role == ExpectedForCentre);
	}

	return true;
}

/**
 * Every degenerate breakout request has a defined answer, and none reads like a healthy brick.
 * A never-solved graph reads Falling (enumerator zero, fail-closed), so bHasSupportAnswer tells it
 * from a collapse; a given joint reads like an unloaded one, so bHasGiven tells them apart. A
 * non-piece gets no rows, so the Max() utilisation for a bad handle cannot leak in. An isolated
 * piece with no joints is still a piece, hence bIsPiece. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceInspectionDegenerateInputsTest,
	"DestructionGame.Core.PieceInspection.DegenerateInputs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceInspectionDegenerateInputsTest::RunTest(const FString& Parameters)
{
	using namespace PieceInspectionTestSupport;

	FStructureBinding Solved;
	BuildWorkedFixture(Solved, /*bPullThePad*/ true, /*bSolve*/ true);

	struct FRefCase
	{
		const TCHAR* Description;
		FPieceRef Ref;
		bool bIsPiece;
	};

	const TArray<FRefCase> RefCases = {
		{ TEXT("a wholly default ref"), FPieceRef(), false },
		{ TEXT("a ref with no structure id"), MakeRef(INDEX_NONE, SubjectPiece), false },
		{ TEXT("a ref with no piece index"), MakeRef(ThisStructure, INDEX_NONE), false },
		{ TEXT("a ref naming somebody else's structure"), MakeRef(SomeOtherStructure, SubjectPiece), false },
		{ TEXT("a ref past the end of the piece array"), MakeRef(ThisStructure, 99), false },
		{ TEXT("a ref with a negative piece index"), MakeRef(ThisStructure, -5), false },
		{ TEXT("a ref naming the piece the player pulled out"), MakeRef(ThisStructure, PulledPadPiece), false },
		{
			// Zero is a valid id; its case is covered by Presenter.PieceMenuRows.
			TEXT("a ref naming a live piece"), MakeRef(ThisStructure, SubjectPiece), true
		},
	};

	for (const FRefCase& Case : RefCases)
	{
		const FPieceInspection Inspection = InspectPiece(Solved, Case.Ref);

		CheckEveryNumberIsUsable(*this, Case.Description, Inspection);

		TestTrue(
			FString::Printf(TEXT("%s should%s resolve to a piece; got %s"),
				Case.Description, Case.bIsPiece ? TEXT("") : TEXT(" not"),
				*DescribeInspection(Inspection)),
			Inspection.bIsPiece == Case.bIsPiece);

		if (Case.bIsPiece)
		{
			continue;
		}

		TestEqual(
			FString::Printf(TEXT("%s should break out no joints at all; got %s"),
				Case.Description, *DescribeInspection(Inspection)),
			Inspection.Joints.Num(), 0);

		TestEqual(
			FString::Printf(TEXT("%s should resolve to no handle; got %s"),
				Case.Description, *DescribeInspection(Inspection)),
			Inspection.PieceIndex, INDEX_NONE);

		TestTrue(
			FString::Printf(TEXT("%s should claim no support answer; got %s"),
				Case.Description, *DescribeInspection(Inspection)),
			!Inspection.bHasSupportAnswer);

		TestTrue(
			FString::Printf(TEXT("%s should fail closed to Falling; got %s"),
				Case.Description, *DescribeInspection(Inspection)),
			Inspection.Support == EPieceSupport::Falling);
	}

	// The same fail-closed answers through the handle overload.
	const FStructure& SolvedStructure = Solved.GetStructure();

	struct FHandleCase
	{
		const TCHAR* Description = nullptr;
		int32 Handle = INDEX_NONE;
	};

	const TArray<FHandleCase> HandleCases = {
		{ TEXT("a handle past the end"), 99 },
		{ TEXT("a negative handle"), -5 },
		{ TEXT("INDEX_NONE as a handle"), INDEX_NONE },
		{ TEXT("the handle of a removed piece"), PulledPadPiece },
	};

	for (const FHandleCase& Case : HandleCases)
	{
		const FPieceInspection Inspection = InspectPiece(SolvedStructure, Case.Handle);

		CheckEveryNumberIsUsable(*this, Case.Description, Inspection);

		TestTrue(
			FString::Printf(TEXT("%s should not be a piece; got %s"),
				Case.Description, *DescribeInspection(Inspection)),
			!Inspection.bIsPiece);

		TestEqual(
			FString::Printf(TEXT("%s should break out no joints; got %s"),
				Case.Description, *DescribeInspection(Inspection)),
			Inspection.Joints.Num(), 0);
	}

	// Never solved: zero loads and Falling; only bHasSupportAnswer shows it is not a collapse.
	FStructureBinding Unsolved;
	BuildWorkedFixture(Unsolved, /*bPullThePad*/ false, /*bSolve*/ false);

	const FPieceInspection Fresh = InspectPiece(Unsolved.GetStructure(), SubjectPiece);

	CheckEveryNumberIsUsable(*this, TEXT("a piece of a never-solved structure"), Fresh);
	CheckInspectionAgreesWithTheGraph(
		*this, TEXT("a piece of a never-solved structure"), Unsolved.GetStructure(), Fresh);

	TestTrue(
		FString::Printf(TEXT("a piece of a never-solved structure is still a piece; got %s"),
			*DescribeInspection(Fresh)),
		Fresh.bIsPiece);

	TestEqual(
		FString::Printf(TEXT("a piece of a never-solved structure still has its joints; got %s"),
			*DescribeInspection(Fresh)),
		Fresh.Joints.Num(), 4);

	TestTrue(
		FString::Printf(
			TEXT("a never-solved structure must say so rather than reading as a collapse; got %s"),
			*DescribeInspection(Fresh)),
		!Fresh.bHasSupportAnswer);

	for (const FJointInspection& Joint : Fresh.Joints)
	{
		TestTrue(
			FString::Printf(
				TEXT("connection %d of a never-solved structure should carry nothing, ")
				TEXT("and be intact while doing it; got %s"),
				Joint.ConnectionIndex, *DescribeInspection(Fresh)),
			FMath::IsNearlyEqual(Joint.ForceUu.Z, 0.0, 1.0e-9)
				&& FMath::IsNearlyEqual(Joint.Utilisation, 0.0, 1.0e-12)
				&& !Joint.bHasGiven);
	}

	// A live piece with no joints: empty list, still a piece, and solved.
	FStructure Lonely;
	const int32 LonelyPad = Lonely.AddPiece(5.0, /*bIsGrounded*/ true);
	Lonely.SolveLoads();

	const FPieceInspection Isolated = InspectPiece(Lonely, LonelyPad);

	CheckEveryNumberIsUsable(*this, TEXT("an isolated grounded pad"), Isolated);
	CheckInspectionAgreesWithTheGraph(*this, TEXT("an isolated grounded pad"), Lonely, Isolated);

	TestTrue(
		FString::Printf(
			TEXT("an isolated grounded pad is a piece with no joints, not an absent one; got %s"),
			*DescribeInspection(Isolated)),
		Isolated.bIsPiece
			&& Isolated.Joints.Num() == 0
			&& Isolated.bHasSupportAnswer
			&& Isolated.Support == EPieceSupport::Grounded);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
