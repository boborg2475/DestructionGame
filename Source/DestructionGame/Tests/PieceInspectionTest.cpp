// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceInspection.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, not anonymous, and named differently from every other one in this
 * directory. An anonymous namespace is private to a TRANSLATION UNIT rather than to a file,
 * and a unity build merges many files into one — at which point two file-local names that
 * collide are a hard compile error between files that never refer to each other. See
 * CURRENT_STATE.md; the `using namespace` lives inside each RunTest body for the same reason.
 */
namespace PieceInspectionTestSupport
{
	using namespace DestructionProfiles;

	/**
	 * Unreal's gravity, transcribed rather than imported.
	 *
	 * The 1 N = 100 uu conversion is ALREADY INSIDE THIS NUMBER — a 2 kg piece weighs
	 * 2 x 980 = 1960 uu, which is 19.6 N x 100 — so applying a factor of 100 anywhere
	 * downstream is the standard way to be wrong by exactly 100x here.
	 */
	constexpr double InspectionGravityCmPerSecondSquared = 980.0;

	/**
	 * SPELLED OUT INDEPENDENTLY, NEVER IMPORTED FROM PRODUCTION.
	 *
	 * 1 N = 100 uu of force and 1 cm2 = 100 mm2, so one megapascal — one newton per square
	 * millimetre — across one square centimetre is 100 x 100 uu. If
	 * DestructionForce::ForceUnitsPerMPaSqCm is ever wrong, this file fails rather than
	 * agreeing with it, which is the entire reason it is not simply used here.
	 */
	constexpr double InspectionForceUnitsPerMPaSqCm = 100.0 * 100.0;

	/** An ordinary bed- or head-joint face. */
	constexpr double InspectionJointAreaSqCm = 100.0;

	/** The structure the fixtures below identify themselves as, and one they do not. */
	constexpr int32 ThisStructure = 4;
	constexpr int32 SomeOtherStructure = 5;

	/*
	 * THE WORKED FIXTURE. Five pieces around one subject, so that the piece under
	 * inspection carries every role at once and one joint that has gone:
	 *
	 *                       [3] Rider  3 kg
	 *                        |  conn 2   bed joint ABOVE the subject
	 *      Pulled [4] ~ ~ ~ [1] Subject 2 kg ---- conn 1 ---- [2] Hanger 1 kg
	 *      (removed) conn 3  |                  head joint     (hangs off the side)
	 *                        |  conn 0   bed joint BENEATH the subject
	 *                       [0] Pad    grounded
	 *
	 * Piece 4 is pulled out before the solve, which SEVERS conn 3 without it ever having
	 * failed — HasGiven true with no break pass, the state DESIGN.md's table calls "went
	 * with a removed piece". That is the joint a readout must not draw like an intact one.
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

	/** Straight up: a bed joint, which bears in compression. */
	const FVector InspectionBedNormal(0.0, 0.0, 1.0);

	/** Straight sideways: a head joint, which can only carry in shear. */
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
	 * The diagram above, built. Solved only if asked, because "nobody has solved yet" is
	 * itself one of the states a readout has to be able to tell apart from a collapse.
	 *
	 * A NULL ACTOR IS FINE HERE. Nothing in this file releases, resolves or destroys one —
	 * the breakout is arithmetic over a graph — and spawning stand-in UObjects for a slice
	 * that never looks at them would be testing the next one by accident.
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
	 * Utilisation of a joint whose ONLY loaded axis is compression.
	 *
	 * Which axis governs is not free, and getting it wrong is how a test aimed at one thing
	 * silently measures another. Here the force is exactly antiparallel to an exactly
	 * vertical normal, so shear and tension are exactly zero and their ratios are exactly
	 * zero whatever their capacities are — compression is the only axis that can be the
	 * worst of the three.
	 */
	double CompressionOnlyUtilisation(
		double ForceMagnitudeUu, double AreaSqCm, const FConnectionStrength& Strength)
	{
		const double StressMPa = ForceMagnitudeUu / (AreaSqCm * InspectionForceUnitsPerMPaSqCm);
		return StressMPa / Strength.CompressiveStrengthMPa;
	}

	/**
	 * Utilisation of a joint whose ONLY loaded axis is shear, with no squeeze on it at all.
	 *
	 * Mohr-Coulomb makes shear capacity cohesion + mu x compressive stress, and compression
	 * here is exactly zero — a vertical load on a vertical face — so the friction term
	 * vanishes and the bare cohesion is the capacity. The one thing that could still take
	 * that away is the truncation cap coming in BELOW the cohesion, which the fixture
	 * precondition in the test asserts cannot happen with the shipped profile.
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

	/** What came back, so a failure reads without a debugger. */
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
				TEXT("%s[c%d->p%d %s Fz=%.6f u=%.9f %s pass=%d]"),
				Index == 0 ? TEXT("") : TEXT(" "),
				Joint.ConnectionIndex,
				Joint.OtherPieceIndex,
				NameOfRole(Joint.Role),
				Joint.ForceUu.Z,
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
	 * Every joint touching a piece, found by brute force over the whole array.
	 *
	 * The oracle for the adjacency half, and it is deliberately the dumbest possible one:
	 * there is no clever way to write this, so the value is not in an independent algorithm
	 * but in the SCOPE — it includes joints that have GIVEN, which the solver's own support
	 * lists drop before the tier is even decided. A breakout that reused a solver-side list
	 * would silently stop showing the joint the player just broke.
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

	/** Nothing a readout shows may be a NaN or an infinity, whatever it was asked about. */
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

			/*
			 * NOT NaN, and finite-or-exactly-Max. TNumericLimits<double>::Max() is the
			 * deliberate fail-closed answer for a joint that is not a joint, so it is
			 * allowed; a NaN is never allowed, because FMath::Max discards it and the
			 * reading silently becomes whichever other axis happened to be lowest.
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
	 * THE ASSERTION THAT STOPS A SECOND COPY OF THE SOLVER GROWING BEHIND THE READOUT.
	 *
	 * Every number on a row is compared EXACTLY — not nearly — against the accessor it must
	 * have come from. Exactness is the whole point: a re-derivation agrees to nine decimal
	 * places forever and differs in the last bit, and CURRENT_STATE.md records that this
	 * project has already paid for exactly that drift twice. A tolerance here would let the
	 * second copy in and then let it disagree quietly.
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

	/** THE ROLE RULE, WRITTEN FROM THE ANGLE RATHER THAN FROM A COSINE.
	 *
	 * DESIGN.md §3: "a joint counts as bearing when its normal is closer to vertical than
	 * horizontal", and the line is at 45 degrees. So the role of a joint TO a piece is
	 * decided by the angle between straight up at that piece and the normal pointing at it —
	 * under 45 it bears, over 135 it is something resting on the piece, and everything
	 * between is a head joint.
	 *
	 * This is deliberately NOT the production expression. FStructure decides by comparing
	 * |normal.Z| against cos 45 and reading a sign; stating the same rule as an angle means
	 * the test and the solver derive the answer two different ways, so the two agreeing is
	 * evidence rather than tautology. It is also what makes the FLIP fall out of one rule
	 * instead of two: the same joint seen from the other end simply has an angle of
	 * 180 minus this one.
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
		 * Whether the expected role is asserted at all.
		 *
		 * FALSE ONLY ON THE BOUNDARY ITSELF. Exactly 45 and exactly 135 degrees are a float
		 * knife-edge: the contract says the comparison is strict, so the boundary reads Head,
		 * but whether a normal built from FMath::Sin and FMath::Cos lands one ulp above or
		 * below cos 45 after normalising is a property of the compiler rather than of the
		 * design. A row whose expected value depends on that is a flake waiting to happen, so
		 * the boundary rows assert only what is genuinely defined: a role comes back, it is
		 * not None, and both ends of the joint agree with FStructure.
		 */
		bool bRoleIsWellDefined = true;
	};
}

/**
 * A PIECE'S PER-JOINT BREAKOUT IS EVERY JOINT TOUCHING IT — INCLUDING THE ONES THAT HAVE
 * GONE — EACH CARRYING THE NEIGHBOUR IT REACHES, WHAT IT IS TO THIS PIECE, WHAT IT IS
 * CARRYING, HOW CLOSE THAT IS TO FAILING, AND WHETHER IT HAS GIVEN; PLUS THE PIECE'S OWN
 * SUPPORT STATE. AND EVERY ONE OF THOSE NUMBERS IS THE SOLVER'S OWN ANSWER READ BACK,
 * NEVER A SECOND DERIVATION OF IT.
 *
 * WHY THE DATA IS TESTED BEFORE ANYTHING DRAWS IT. The piece menu widget was landed under an
 * explicit recorded exception to the TDD gate, on the condition that it holds no logic at all
 * — no branch, no filter, no computation — because a code-built test world has no viewport
 * and no headless assertion can see a button. A debugger readout is precisely the feature
 * that would erode that condition, so every number it will show is decided here, in Core,
 * where a test can reach it.
 *
 * WHY THE ASSERTIONS ARE SPLIT IN TWO, AND WHY BOTH ARE NEEDED. The table pins the actual
 * numbers, hand-derived from published mortar strengths and from a conversion spelled out
 * independently in this file — that is what catches the whole chain being wrong, and in
 * particular the 100x slip that a stray newton conversion would produce. The agreement
 * sweep then pins that those numbers came from FStructure's own accessors, EXACTLY, bit for
 * bit. A table alone would pass against a hand-copied re-derivation that happens to be
 * right today; an agreement sweep alone would pass against two identical copies of a wrong
 * answer. Only the pair says both things.
 *
 * WHICH AXIS GOVERNS IS WORKED THROUGH FOR EVERY JOINT AND WRITTEN DOWN. ComputeUtilisation
 * returns the worst of three axes, so a joint aimed at shear silently measures compression
 * if the compression happens to utilise more. Every bed joint here takes a load exactly
 * antiparallel to an exactly vertical normal, so shear and tension are exactly zero and
 * compression is the only axis that can win; the head joint takes a load exactly parallel to
 * its face, so compression and tension are exactly zero and shear is the only one. The
 * fixture precondition below asserts the one remaining way that could change — mortar's
 * shear truncation cap dropping below its cohesion.
 *
 * NEEDS A TICKING WORLD: no, and not even a world. A structure is a graph and a breakout is
 * a scan of it, so this runs at arithmetic speed beside the solver tests.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceInspectionJointBreakoutTest,
	"DestructionGame.Core.PieceInspection.JointBreakout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceInspectionJointBreakoutTest::RunTest(const FString& Parameters)
{
	using namespace PieceInspectionTestSupport;

	/*
	 * FIXTURE PRECONDITION, and it is what keeps the head joint measuring shear. With no
	 * compression on the joint the Mohr-Coulomb friction term is zero, so capacity is
	 * min(cohesion, cap) — which is the cohesion only while the cap sits above it. A retune
	 * that dropped the ceiling below the bond would silently change what the head-joint row
	 * is measuring, and this says so rather than letting it slide.
	 */
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
	 * THE HAND-DERIVED LOADS.
	 *
	 * The hanger has no bed joint beneath it, so it falls back to its head joint and pushes
	 * its whole weight sideways into the subject. The rider rests squarely on the subject.
	 * The subject therefore carries its own 2 kg plus the 3 kg above it plus the 1 kg hanging
	 * off it, and — with the pulled pad's joint severed — the one bed joint left underneath
	 * takes all six of them.
	 */
	const double HangerLoadUu = HangerMassKg * InspectionGravityCmPerSecondSquared;
	const double RiderLoadUu = RiderMassKg * InspectionGravityCmPerSecondSquared;
	const double PadLoadUu =
		(SubjectMassKg + HangerMassKg + RiderMassKg) * InspectionGravityCmPerSecondSquared;

	/*
	 * SIGNED BY WHICH END IS BEING HELD UP. ConnectionLoad.h's convention is that a joint's
	 * force is the force acting on PieceB, and every joint in this fixture names the loaded
	 * piece second, so every one of them carries its share straight down.
	 */
	/*
	 * FIXTURE PRECONDITIONS, AND THEY ARE HERE BECAUSE OF A TRAP THIS PROJECT HAS ALREADY
	 * BEEN CAUGHT BY.
	 *
	 * Every row below asserts that the breakout REPORTS a number; against an implementation
	 * that hands back no rows at all, all of them fail for the same uninteresting reason and
	 * the arithmetic is never reached — CURRENT_STATE.md records exactly that happening to
	 * MenuOffersWhatCanRun's rejection rows, which passed vacuously against a stub. These
	 * ask the graph directly, so the hand-derived loads and utilisations are checked against
	 * the solver whatever the breakout does, and a fixture that stops producing them says so
	 * in its own words rather than through twenty confusing row failures.
	 *
	 * They are green on arrival and are meant to be. They drive nothing; they are what makes
	 * the rows that DO drive something trustworthy.
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

	/*
	 * AND THE SEVERED ONE READS EXACTLY LIKE AN INTACT UNLOADED ONE ON BOTH NUMBERS, which is
	 * the whole reason bHasGiven has to be a field on the row. If this ever stops being true
	 * the "must not draw them alike" argument weakens and the row below should be rethought
	 * rather than retuned.
	 */
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
			/*
			 * THE SUBJECT, AND THE WHOLE REASON THE FIXTURE IS SHAPED THIS WAY: one piece
			 * wearing all three roles at once plus a joint that went with a removed
			 * neighbour. A breakout that reported only a piece's SUPPORTS would show one row
			 * here instead of four, and would look entirely reasonable doing it.
			 */
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
					 * WENT WITH A REMOVED PIECE: given, but never stamped, because it did not
					 * fail — it was deleted, and DESIGN.md is explicit that it must not appear
					 * in the sequence a collapse is replayed from. It reads zero force at zero
					 * utilisation, which is exactly what an intact unloaded joint reads, and
					 * bHasGiven is the only thing telling them apart. It still names piece 4
					 * and still reports the tier it used to be.
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
			/*
			 * THE PULLED PAD IS NOT A PIECE ANY MORE, and it gets no joints and no support
			 * state — not even the stale one FStructure legitimately keeps until the next
			 * solve. A solver accessor with a documented scope may hand back last solve's
			 * answer; a readout drawn beside a brick that is gone may not.
			 */
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

			/*
			 * THE ORDER IS PART OF THE CONTRACT. A list that reshuffled between two looks at
			 * the same brick is unreadable, and comparing row by row against an ordered
			 * expectation is what checks identity and ordering in one comparison.
			 */
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

			/*
			 * UNREAL FORCE UNITS, UNCONVERTED. A readout that turned these into newtons on
			 * the way past would be a second, open-coded conversion boundary — forbidden by
			 * CLAUDE.md — and it would be out by exactly 100x here, which is loud rather than
			 * plausible only because the expected value is hand-derived as mass x 980.
			 */
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

	/*
	 * COMPLETENESS, over every live piece rather than over the ones the table names: the set
	 * of connections a piece breaks out is exactly the set of connections that touch it,
	 * given-or-not, in ascending order. The table above says what the interesting pieces
	 * carry; this says nothing was quietly left out of any of them.
	 */
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

	/*
	 * THE REF-ENTERED OVERLOAD IS THE HANDLE ONE PLUS A RESOLVE, and nothing else. The
	 * comparison is over the whole answer rather than over a field, because a second entry
	 * point that agreed on the support state and disagreed on a joint would be the worst of
	 * both.
	 */
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
	 * AND THE OTHER WAY A JOINT LEAVES A STRUCTURE: IT FAILED, AND THE PASS THAT BROKE IT IS
	 * THE ONLY RECORD OF WHEN.
	 *
	 * Deliberately its own two-piece structure rather than a sixth spoke on the fixture
	 * above, because it needs SolveAndBreak — which FStructureBinding does not expose (the
	 * cascade is not on the world wire yet) — and because a fixture that cascades is a
	 * fixture whose hand-derived loads move if any of its joints ever starts giving. One
	 * grossly overloaded joint, one pass, one stamp.
	 */
	FStructure Cascaded;
	const int32 CascadePad = Cascaded.AddPiece(1.0, /*bIsGrounded*/ true);
	const int32 CascadeLoad = Cascaded.AddPiece(200.0);

	FConnection Overloaded;
	Overloaded.PieceA = CascadePad;
	Overloaded.PieceB = CascadeLoad;
	Overloaded.InterfaceNormal = InspectionBedNormal;

	/*
	 * ONE SQUARE CENTIMETRE UNDER 200 KG: 196,000 uu over 1 cm2 is 19.6 MPa against mortar's
	 * 10 MPa in compression, so it gives at 1.96 in the first pass. Compression is again the
	 * only loaded axis, so nothing else can govern.
	 */
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
		/*
		 * A JOINT THAT FAILED UNDER LOAD AND A JOINT THAT WENT WITH A REMOVED PIECE READ
		 * IDENTICALLY ON EVERY OTHER FIELD — both given, both carrying nothing, both at zero
		 * utilisation. The pass number is the entire difference, and it is what phase 5
		 * replays a collapse from.
		 */
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

	return true;
}

/**
 * THE ROLE ON A BREAKOUT ROW IS THE SOLVER'S OWN TWO-TIER DECISION, FOR THE PIECE BEING
 * INSPECTED, AND IT FLIPS WHEN THE SAME JOINT IS LOOKED AT FROM THE OTHER END.
 *
 * WHY THIS IS WORTH A TEST OF ITS OWN. The bed/head distinction is the single most
 * load-bearing classification in the solver: DESIGN.md records that routing which ignored it
 * gave a keystone that bore none of the wall above it, at 0.010 utilisation, with nothing
 * crashing and nothing moving — the wall just stood there being wrong. A debugger that cannot
 * show it hides exactly the thing a player would be trying to understand. And because the
 * decision lived inside SolveLoads as a file-local enum, a readout is the first thing that
 * would have had to COPY it, which is the drift this file exists to prevent.
 *
 * THE EXPECTATION IS DERIVED FROM THE ANGLE, NOT FROM THE COSINE. FStructure compares
 * |normal.Z| against cos 45 and reads a sign; the oracle here states DESIGN.md's rule as an
 * angle and gets the flip for free by measuring it from each end in turn. The two agreeing is
 * therefore evidence rather than a tautology, which a transcribed constant would be.
 *
 * A STAR RATHER THAN A TEST PER ANGLE: one centre piece and one neighbour per angle, so
 * adding an angle is a row and one inspection of the centre yields the whole sweep.
 *
 * DELIBERATELY UNSOLVED. A role is pure geometry and needs no load, so a breakout must be
 * able to say what a joint IS before anything has said what it carries — which is also the
 * state a player is in immediately after laying a brick.
 *
 * NEEDS A TICKING WORLD: no, and not even a world.
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

		/*
		 * The normal points at the SPOKE, so the angle toward the spoke is the case's own and
		 * the angle toward the centre is its supplement. One rule, applied from each end.
		 */
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

		/*
		 * ALWAYS ASSERTED, INCLUDING ON THE BOUNDARY ROWS. None is what a joint that is not
		 * on this piece reports, and a breakout that answered None for a joint it is listing
		 * would be showing a row with no tier at all.
		 */
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

		/*
		 * THE FLIP. The same joint from the other end is the mirror role, and Head is its own
		 * mirror. A classifier that read the normal without asking which end it was being
		 * asked about would report the same role to both pieces — which is a joint that bears
		 * its own load in both directions, and is how a wall stands up while being wrong.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s (%.0f deg): to the centre the same joint should be %s; it reports %s"),
				Case.Description, Case.AngleDeg, NameOfRole(ExpectedForCentre), NameOfRole(FromCentre.Role)),
			FromCentre.Role == ExpectedForCentre);
	}

	return true;
}

/**
 * EVERY DEGENERATE WAY OF ASKING FOR A BREAKOUT HAS A DEFINED ANSWER, AND NONE OF THEM READS
 * LIKE A HEALTHY BRICK.
 *
 * THE POLARITY IS THE POINT, AND IT IS THE ONE THIS CODEBASE KEEPS PAYING FOR. Two different
 * pairs of states are indistinguishable on the numbers alone and must not be drawn alike:
 *
 *   - A NEVER-SOLVED graph reports every piece Falling, because Falling is enumerator zero so
 *     that an absent answer cannot claim the structure is resting on the earth. A readout that
 *     showed a freshly built wall as a column of falling bricks would be reporting a
 *     catastrophe that has not happened, so bHasSupportAnswer has to be on the struct.
 *   - A joint that has GIVEN carries no force, so it reads 0 uu at 0 utilisation — exactly
 *     what an intact unloaded joint reads. bHasGiven is the only thing separating them.
 *
 * And the third answer in the family is the one that must never reach a row at all:
 * GetConnectionUtilisation answers TNumericLimits<double>::Max() for a handle that names no
 * joint, deliberately, because a zero there would read as unloaded and perfectly healthy. A
 * breakout only ever lists real connection indices, so that answer has no route in — which is
 * checked here by there being no rows at all for anything that is not a piece.
 *
 * A LIVE PIECE WITH NO JOINTS IS A REAL THING and must not read as a ref that names nothing.
 * An isolated grounded pad has an empty list and is still a piece; that is why bIsPiece is a
 * field rather than an inference from Joints.Num().
 *
 * NEEDS A TICKING WORLD: no, and not even a world.
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
			/*
			 * ZERO IS A REAL STRUCTURE AND A REAL PIECE, so the sentinel is INDEX_NONE and
			 * never falsiness — but this fixture is structure 4, so the live row here is the
			 * subject. The zero case is covered by Presenter.PieceMenuRows one layer up.
			 */
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

	/* The same fail-closed answers by handle, since that overload is entered directly too. */
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

	/*
	 * THE NEVER-SOLVED GRAPH. Same wall, nothing pulled and nothing solved: a real piece with
	 * real joints, every one of them carrying a genuine zero, and the support state reading
	 * Falling because that is what "nobody has asked" looks like. The ONLY thing that can tell
	 * a player this is not a collapse is bHasSupportAnswer.
	 */
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

	/*
	 * A LIVE PIECE WITH NOTHING JOINED TO IT. Empty list, still a piece, and a solve HAS
	 * answered for it — the three readings a ref naming nothing must not share.
	 */
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
