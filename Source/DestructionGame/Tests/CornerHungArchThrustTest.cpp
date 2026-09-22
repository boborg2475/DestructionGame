// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A corner-hung arch must thrust within its wall plane. ReseatSpannedGroups used the vector from
 * the span to the first abutment's centre of mass as the thrust axis. When the abutments are
 * perpendicular walls, that vector points diagonally inboard, pushing each springing out of the
 * wall plane (the shed probe measured about +/-30,732 uu in Y). The correct axis is the in-plane
 * difference of the two end centres.
 *
 *     plan view (Y = out of the back-wall plane):
 *
 *          X: 9.5   29.5 30.5    50.5 51.5   71.5
 *      Y=60  +----AL----+  +--S--+  +----AR----+     <- back-wall plane (S) at Y in [50,60]
 *            |          |  (span) |          |
 *            |  side    |         |   side   |
 *      Y=30  +   wall   +         +   wall   +        <- stubs run inboard to Y=30
 *
 * Span to AL = (-21, -10, 0), 43% out of plane; AL - AR = (-42, 0, 0), in plane.
 *
 * Asserts: the fixture is corner-hung; the arch fired (equal and opposite X at the springing
 * seats); each seat carries zero Y, checked per seat since the two cancel as a net; the span
 * stays Supported. Loads are vertical and head joints shear in Y-Z, so any seat Y is thrust.
 * World-free. Named namespace for unity builds.
 */
namespace CornerHungArchThrustTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** With 1 uu = 1 cm and mass in kg, MassKg * 980 is a force in uu. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** A box from its two opposite corners. */
	FPieceBox BoxFromBounds(
		double LoX, double HiX, double LoY, double HiY, double LoZ, double HiZ)
	{
		FPieceBox Box;
		Box.CentreCm = FVector((LoX + HiX) / 2.0, (LoY + HiY) / 2.0, (LoZ + HiZ) / 2.0);
		Box.ExtentCm = FVector((HiX - LoX) / 2.0, (HiY - LoY) / 2.0, (HiZ - LoZ) / 2.0);
		return Box;
	}

	/** Clay-brick mass, kg: density (g/cm3) * volume (cm3) / 1000. */
	double BoxMassKg(const FPieceBox& Box)
	{
		return ClayBrick.DensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	// Geometry in cm; Y is out of the back-wall plane.
	constexpr double JointCm = 1.0;

	/** Grounded foundation course, and the springing course one joint above it. */
	constexpr double FoundationLoZ = 0.0;
	constexpr double FoundationHiZ = 6.5;
	constexpr double CourseLoZ = 7.5;   // 1 cm bed joint above the foundation
	constexpr double CourseHiZ = 14.0;

	/** The spanning back-wall brick S. */
	constexpr double SpanLoX = 30.5;
	constexpr double SpanHiX = 50.5;
	constexpr double SpanLoY = 50.0;   // the back-wall plane the arch must thrust within: Y in [50, 60]
	constexpr double SpanHiY = 60.0;

	/** Side-wall stubs run inboard to Y = 30, so their centres of mass (Y = 45) sit out of plane. */
	constexpr double SideWallLoY = 30.0;
	constexpr double SideWallHiY = 60.0;

	constexpr double LeftWallLoX = 9.5;
	constexpr double LeftWallHiX = 29.5;   // right face 1 cm left of the span's left face at 30.5
	constexpr double RightWallLoX = 51.5;  // left face 1 cm right of the span's right face at 50.5
	constexpr double RightWallHiX = 71.5;

	struct FCornerArchFixture
	{
		FStructure Structure;
		int32 Span = INDEX_NONE;
		int32 LeftWall = INDEX_NONE;
		int32 RightWall = INDEX_NONE;
		int32 LeftFoundation = INDEX_NONE;
		int32 RightFoundation = INDEX_NONE;
		int32 LeftSpringingSeat = INDEX_NONE;    // the bed joint LeftWall <-> LeftFoundation
		int32 RightSpringingSeat = INDEX_NONE;   // the bed joint RightWall <-> RightFoundation
		FVector SpanCentreCm = FVector::ZeroVector;
		FVector LeftWallCentreCm = FVector::ZeroVector;
		FVector RightWallCentreCm = FVector::ZeroVector;
	};

	/** Lay the fixture. Joint order makes the left wall the first abutment the reseat finds. */
	bool Build(FCornerArchFixture& Out)
	{
		const FPieceBox LeftFoundationBox =
			BoxFromBounds(LeftWallLoX, LeftWallHiX, SideWallLoY, SideWallHiY, FoundationLoZ, FoundationHiZ);
		const FPieceBox RightFoundationBox =
			BoxFromBounds(RightWallLoX, RightWallHiX, SideWallLoY, SideWallHiY, FoundationLoZ, FoundationHiZ);
		const FPieceBox LeftWallBox =
			BoxFromBounds(LeftWallLoX, LeftWallHiX, SideWallLoY, SideWallHiY, CourseLoZ, CourseHiZ);
		const FPieceBox RightWallBox =
			BoxFromBounds(RightWallLoX, RightWallHiX, SideWallLoY, SideWallHiY, CourseLoZ, CourseHiZ);
		const FPieceBox SpanBox =
			BoxFromBounds(SpanLoX, SpanHiX, SpanLoY, SpanHiY, CourseLoZ, CourseHiZ);

		Out.SpanCentreCm = SpanBox.CentreCm;
		Out.LeftWallCentreCm = LeftWallBox.CentreCm;
		Out.RightWallCentreCm = RightWallBox.CentreCm;

		Out.LeftFoundation =
			Out.Structure.AddPiece(BoxMassKg(LeftFoundationBox), /*bIsGrounded*/ true, LeftFoundationBox.CentreCm);
		Out.RightFoundation =
			Out.Structure.AddPiece(BoxMassKg(RightFoundationBox), /*bIsGrounded*/ true, RightFoundationBox.CentreCm);
		Out.LeftWall =
			Out.Structure.AddPiece(BoxMassKg(LeftWallBox), /*bIsGrounded*/ false, LeftWallBox.CentreCm);
		Out.RightWall =
			Out.Structure.AddPiece(BoxMassKg(RightWallBox), /*bIsGrounded*/ false, RightWallBox.CentreCm);
		Out.Span =
			Out.Structure.AddPiece(BoxMassKg(SpanBox), /*bIsGrounded*/ false, SpanBox.CentreCm);

		FConnection Joint;

		// The springing seats: bed joints from each side wall to its foundation.
		if (!MakeInterface(Out.LeftFoundation, LeftFoundationBox, Out.LeftWall, LeftWallBox,
				JointCm, GeneralPurposeMortar, Joint))
		{
			return false;
		}
		Out.LeftSpringingSeat = Out.Structure.AddConnection(Joint);

		if (!MakeInterface(Out.RightFoundation, RightFoundationBox, Out.RightWall, RightWallBox,
				JointCm, GeneralPurposeMortar, Joint))
		{
			return false;
		}
		Out.RightSpringingSeat = Out.Structure.AddConnection(Joint);

		// Head joints from the span to the side walls, left first.
		if (!MakeInterface(Out.LeftWall, LeftWallBox, Out.Span, SpanBox,
				JointCm, GeneralPurposeMortar, Joint))
		{
			return false;
		}
		Out.Structure.AddConnection(Joint);

		if (!MakeInterface(Out.RightWall, RightWallBox, Out.Span, SpanBox,
				JointCm, GeneralPurposeMortar, Joint))
		{
			return false;
		}
		Out.Structure.AddConnection(Joint);

		return true;
	}

	int32 StrandedCount(const FStructure& S)
	{
		int32 N = 0;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (!S.IsPieceRemoved(P) && S.GetPieceSupport(P) == EPieceSupport::Stranded)
			{
				++N;
			}
		}
		return N;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCornerHungArchThrustTest,
	"DestructionGame.Core.Structure.ACornerHungArchThrustsInThePlaneOfItsWall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCornerHungArchThrustTest::RunTest(const FString& Parameters)
{
	using namespace CornerHungArchThrustTestSupport;
	using namespace DestructionProfiles;

	TestEqual(
		TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries"),
		ClayBrick.DensityGramsPerCubicCm, 1.9);

	FCornerArchFixture Fixture;
	if (!Build(Fixture))
	{
		AddError(TEXT("FIXTURE: the corner-hung arch failed to lay — a MakeInterface face test refused a joint."));
		return false;
	}

	TestEqual(TEXT("FIXTURE: five pieces — span, two side walls, two foundations"),
		Fixture.Structure.NumPieces(), 5);
	TestEqual(TEXT("FIXTURE: four joints — two springing seats and two head joints"),
		Fixture.Structure.NumConnections(), 4);

	// Precondition: the span-to-first-abutment vector (the old thrust axis) is out of plane.
	const FVector TowardFirstAbutment = Fixture.LeftWallCentreCm - Fixture.SpanCentreCm;
	const FVector EndCentreDifference = Fixture.LeftWallCentreCm - Fixture.RightWallCentreCm;

	const double OutOfPlaneFraction =
		FMath::Abs(TowardFirstAbutment.Y) / TowardFirstAbutment.Size();

	AddInfo(FString::Printf(
		TEXT("FIXTURE: TowardAbutmentCm[0] = (%.3f, %.3f, %.3f), |Y|/|v| = %.4f out of plane; ")
		TEXT("EndCentreCm[0]-[1] = (%.3f, %.3f, %.3f), the in-plane axis the fix should use."),
		TowardFirstAbutment.X, TowardFirstAbutment.Y, TowardFirstAbutment.Z, OutOfPlaneFraction,
		EndCentreDifference.X, EndCentreDifference.Y, EndCentreDifference.Z));

	TestTrue(
		*FString::Printf(
			TEXT("FIXTURE: the first abutment vector is substantially OUT OF PLANE (|Y|/|v| = %.4f, ")
			TEXT("must exceed 0.2) — this is the corner-hung geometry the defect needs"),
			OutOfPlaneFraction),
		OutOfPlaneFraction > 0.2);

	TestTrue(
		TEXT("FIXTURE: the two end centres sit at the SAME out-of-plane Y, so the correct thrust axis ")
		TEXT("(their difference) is purely in plane"),
		FMath::IsNearlyZero(EndCentreDifference.Y, 1e-9));

	Fixture.Structure.SolveLoads();

	const EPieceSupport SpanSupport = Fixture.Structure.GetPieceSupport(Fixture.Span);
	const int32 Stranded = StrandedCount(Fixture.Structure);

	AddInfo(FString::Printf(
		TEXT("OUTCOME: span support = %d (1 Grounded, 2 Supported); %d stranded."),
		static_cast<int32>(SpanSupport), Stranded));

	TestTrue(TEXT("OUTCOME: the span reads Supported — the arch holds"),
		SpanSupport == EPieceSupport::Supported);
	TestEqual(TEXT("OUTCOME: nothing is stranded"), Stranded, 0);

	// Force on each seat's PieceB: X in plane, Y out of plane, Z not asserted.
	const FVector LeftSeatForce = Fixture.Structure.GetConnectionForce(Fixture.LeftSpringingSeat);
	const FVector RightSeatForce = Fixture.Structure.GetConnectionForce(Fixture.RightSpringingSeat);

	AddInfo(FString::Printf(
		TEXT("LEFT springing seat force  = (%.1f, %.1f, %.1f) uu   [X in-plane, Y out-of-plane, Z vertical]"),
		LeftSeatForce.X, LeftSeatForce.Y, LeftSeatForce.Z));
	AddInfo(FString::Printf(
		TEXT("RIGHT springing seat force = (%.1f, %.1f, %.1f) uu"),
		RightSeatForce.X, RightSeatForce.Y, RightSeatForce.Z));

	// Guard: the arch fired, or the zero-Y checks below would be vacuous.
	TestTrue(
		*FString::Printf(
			TEXT("GUARD: the arch fired — the LEFT springing carries a non-zero IN-PLANE (X) thrust ")
			TEXT("(%.1f uu)"),
			LeftSeatForce.X),
		FMath::Abs(LeftSeatForce.X) > 1.0);

	TestTrue(
		*FString::Printf(
			TEXT("GUARD: the two springings push APART — in-plane X equal and opposite (L %.1f, R %.1f)"),
			LeftSeatForce.X, RightSeatForce.X),
		FMath::IsNearlyEqual(LeftSeatForce.X, -RightSeatForce.X, 1.0));

	// A flat arch puts zero Y on each springing (the diagonal axis put about +/-15,000 uu).
	TestTrue(
		*FString::Printf(
			TEXT("A flat arch cannot push its LEFT springing OUT OF THE WALL PLANE: the out-of-plane ")
			TEXT("(Y) force must be zero, but it reads %.1f uu"),
			LeftSeatForce.Y),
		FMath::IsNearlyZero(LeftSeatForce.Y, 1.0e-2));

	TestTrue(
		*FString::Printf(
			TEXT("A flat arch cannot push its RIGHT springing OUT OF THE WALL PLANE: the out-of-plane ")
			TEXT("(Y) force must be zero, but it reads %.1f uu"),
			RightSeatForce.Y),
		FMath::IsNearlyZero(RightSeatForce.Y, 1.0e-2));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
