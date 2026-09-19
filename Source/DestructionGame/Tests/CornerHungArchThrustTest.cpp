// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A SPANNED ARCH TAKES ITS THRUST DIRECTION FROM THE WRONG VECTOR, SO A CORNER-HUNG WALL PUSHES
 * ITS SPRINGINGS OUT OF THE WALL PLANE — a force a flat, in-plane arch cannot produce.
 *
 * THE DEFECT. `FStructure::ReseatSpannedGroups` records a spanned opening as an `FSpannedArch`
 * and hands `ApplyArchingThrust` `Arch.TowardEndZero = TowardAbutmentCm[0]` (Core/Structure.cpp)
 * — the raw vector from the spanned group's centre to the FIRST abutment's centre of mass. For a
 * normal in-plane opening that vector is purely in the wall plane. But when a spanned group
 * re-seats onto PERPENDICULAR walls — the corner-hung case, e.g. the realistic shed's back wall
 * arching into its two side walls after a deep band is removed — the first abutment sits
 * diagonally toward one corner, inboard of the wall plane, so `TowardAbutmentCm[0]` carries an
 * out-of-plane component and `ApplyArchingThrust` pushes each springing along that diagonal axis
 * (the shed probe measured about +/-30,732 uu of it in Y).
 *
 * The two END CENTRES computed a few lines down (`EndCentreCm[0]`/`[1]`, used for `Arch.SpanCm`)
 * have out-of-plane components that CANCEL between the two ends — both sit at the same back-wall
 * Y — so the correct thrust axis is
 * `normalize(project_to_horizontal_seat_plane(EndCentreCm[0] - EndCentreCm[1]))`, purely along the
 * wall run. THIS TEST DOES NOT WRITE THAT FIX; it pins the behaviour it must produce: a
 * corner-hung springing carries no out-of-plane force.
 *
 * THE FIXTURE (world-free). A single back-wall brick S spans a hole and abuts, across a head
 * joint at each end, a long side-wall stub running PERPENDICULAR to it (inboard in -Y). Each stub
 * rests on a grounded foundation by a bed joint — the SPRINGING SEAT the arch delivers its thrust
 * into and the joint this test reads.
 *
 *     plan view (X across, Y into the page = out of the back-wall plane):
 *
 *          X: 9.5   29.5 30.5    50.5 51.5   71.5
 *      Y=60  +----AL----+  +--S--+  +----AR----+     <- back-wall plane (S) at Y in [50,60]
 *            |          |  (span) |          |
 *            |  side    |         |   side   |
 *      Y=30  +   wall   +         +   wall   +        <- stubs run inboard to Y=30
 *
 *   group centre = S CoM = (40.5, 55).  AL CoM = (19.5, 45).  AR CoM = (61.5, 45).
 *
 *   TowardAbutmentCm[0] = AL - group = (-21, -10, 0)   <- has a -Y (out-of-plane) component.
 *   EndCentreCm[0] - EndCentreCm[1]  = AL - AR = (-42, 0, 0)  <- purely along the wall run (X).
 *
 * So the buggy axis normalises to (-0.903, -0.430, 0) — 43% out of plane — against the fix's
 * (-1, 0, 0), purely in plane. The abutments still split into two ends (dot product -341 < 0), so
 * the arch genuinely fires.
 *
 * WHAT IS ASSERTED, AND WHY EACH FORM:
 *
 *   - THE FIXTURE IS GENUINELY CORNER-HUNG. `TowardAbutmentCm[0]`, rebuilt here from the piece
 *     positions the solver reads, has an out-of-plane (Y) component that is a large fraction of
 *     its length — the precondition the defect needs, and a statement about the geometry alone.
 *
 *   - THE ARCH FIRED. Each springing seat carries a non-zero in-plane (X) thrust, equal and
 *     opposite between the two ends — the guard against the out-of-plane row being vacuously true.
 *
 *   - THE RED CLAIM: each springing seat carries ZERO out-of-plane (Y) force. Asserted PER
 *     SPRINGING, never as a net: the two ends are equal and opposite, so their spurious Y cancels
 *     globally even today, and only the per-joint reading discriminates broken from fixed. Today
 *     each springing reads about +/-15,000 uu of Y; the correct answer is zero.
 *
 *   - IT STILL STANDS. The span reads Supported and nothing is stranded — the fix removes only
 *     the out-of-plane part, not whether the arch holds.
 *
 * NEVER A DISPLACEMENT: the claim is a force component on a named joint, decomposed into the wall
 * plane and its perpendicular; the outcome claim is a support classification and a stranded count.
 *
 * WHY Y IS PURELY THE THRUST. Every applied load here is vertical (gravity, -Z), and the only
 * non-vertical joint is the X-normal head joint, whose shear plane is Y-Z — the span's weight
 * resolves as pure -Z shear there, so the abutment receives no Y from routing and the springing
 * bed joint's Y component is exactly and only whatever the arch thrust put there.
 *
 * NEEDS A TICKING WORLD: no. FStructure is plain arithmetic over a graph; gravity is the piece
 * weights the solver applies itself.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit, at which
 * point two anonymous namespaces are the same namespace and identically-named helpers collide.
 */
namespace CornerHungArchThrustTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* ---- units, spelled out here rather than imported so a wrong constant fails the test ---- */

	/** In a world where 1 uu = 1 cm and mass is kg, MassKg * 980 IS a force in uu. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** A box the size of a whole brick, from its two opposite corners. */
	FPieceBox BoxFromBounds(
		double LoX, double HiX, double LoY, double HiY, double LoZ, double HiZ)
	{
		FPieceBox Box;
		Box.CentreCm = FVector((LoX + HiX) / 2.0, (LoY + HiY) / 2.0, (LoZ + HiZ) / 2.0);
		Box.ExtentCm = FVector((HiX - LoX) / 2.0, (HiY - LoY) / 2.0, (HiZ - LoZ) / 2.0);
		return Box;
	}

	/** The mass of a clay-brick box: density (g/cm3) * volume (cm3) / 1000. */
	double BoxMassKg(const FPieceBox& Box)
	{
		return ClayBrick.DensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	/* ---- the geometry, all in centimetres; Y is the OUT-OF-PLANE axis of the back wall ---- */

	constexpr double JointCm = 1.0;

	/** The foundation course, grounded; the springing course rests on it a mortar joint up. */
	constexpr double FoundationLoZ = 0.0;
	constexpr double FoundationHiZ = 6.5;
	constexpr double CourseLoZ = 7.5;   // 1 cm bed joint above the foundation
	constexpr double CourseHiZ = 14.0;

	/** The spanning back-wall brick S: a hole beneath it, head joints to the side walls at its ends. */
	constexpr double SpanLoX = 30.5;
	constexpr double SpanHiX = 50.5;
	constexpr double SpanLoY = 50.0;   // the back-wall plane the arch must thrust within: Y in [50, 60]
	constexpr double SpanHiY = 60.0;

	/**
	 * The side-wall stubs run PERPENDICULAR to the back wall, inboard to Y = 30, so their centres of
	 * mass sit at Y = 45 — well OUT of the back-wall plane the span occupies (Y in [50, 60]). That
	 * inboard offset is exactly what gives TowardAbutmentCm[0] its spurious out-of-plane component.
	 */
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

	/**
	 * Lay the corner-hung arch. Joints are added in an order that makes the LEFT side wall the first
	 * abutment the reseat discovers (so TowardAbutmentCm[0] is the left, inboard-diagonal vector),
	 * matching the geometry the header derives.
	 */
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

		/* The two SPRINGING SEATS first — bed joints from each side wall down to its foundation. */
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

		/* Then the two HEAD joints from the span to the side walls — LEFT before RIGHT. */
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

	/* The fixture's numbers are ratios of published values, asserted rather than imported: a test
	 * that read the profile back would agree with a wrong profile. */
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

	/*
	 * THE FIXTURE IS GENUINELY CORNER-HUNG — TowardAbutmentCm[0] IS OUT OF PLANE. Rebuilt here
	 * from the piece positions the solver reads: the vector from the spanned group (the span
	 * brick) to the first abutment (the left side wall). The reseat records exactly this as
	 * `Arch.TowardEndZero` and hands it to the thrust pass as the push axis.
	 */
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

	/* The arch must hold — the fix removes only the out-of-plane push, not the hold. */
	const EPieceSupport SpanSupport = Fixture.Structure.GetPieceSupport(Fixture.Span);
	const int32 Stranded = StrandedCount(Fixture.Structure);

	AddInfo(FString::Printf(
		TEXT("OUTCOME: span support = %d (1 Grounded, 2 Supported); %d stranded."),
		static_cast<int32>(SpanSupport), Stranded));

	TestTrue(TEXT("OUTCOME: the span reads Supported — the arch holds"),
		SpanSupport == EPieceSupport::Supported);
	TestEqual(TEXT("OUTCOME: nothing is stranded"), Stranded, 0);

	/*
	 * The springing forces, decomposed into the wall plane (X) and its perpendicular (Y).
	 * GetConnectionForce is the force acting on the joint's PieceB; Z is gravity and this test
	 * says nothing about it.
	 */
	const FVector LeftSeatForce = Fixture.Structure.GetConnectionForce(Fixture.LeftSpringingSeat);
	const FVector RightSeatForce = Fixture.Structure.GetConnectionForce(Fixture.RightSpringingSeat);

	AddInfo(FString::Printf(
		TEXT("LEFT springing seat force  = (%.1f, %.1f, %.1f) uu   [X in-plane, Y out-of-plane, Z vertical]"),
		LeftSeatForce.X, LeftSeatForce.Y, LeftSeatForce.Z));
	AddInfo(FString::Printf(
		TEXT("RIGHT springing seat force = (%.1f, %.1f, %.1f) uu"),
		RightSeatForce.X, RightSeatForce.Y, RightSeatForce.Z));

	/* The arch fired — the guard that stops the out-of-plane row being vacuous: each springing
	 * must carry a real in-plane thrust, and the two ends push apart (equal and opposite in X). */
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

	/*
	 * THE RED — a flat arch's thrust lies in the wall plane, so each springing carries zero
	 * out-of-plane (Y) force. Today the diagonal TowardEndZero axis pushes about +/-15,000 uu of
	 * it into each springing; the correct answer is zero.
	 */
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
