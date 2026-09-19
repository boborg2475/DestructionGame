// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Tests/StaircaseWallTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The staircase's arithmetic, with no world and nothing broken: cutting the void puts the bottom
 * of the corbel at 0.0527 of capacity and not one of its eleven steps past the line. (0.369
 * before the 2026-08-14 mean re-anchor flip moved the flexural bond 0.10 -> 0.70.)
 *
 * It used to say 22.9 and eight of eleven; the change is the user's ruling of 2026-08-06, not a
 * tuning. A brick deleted at a free end must not bring the wall down, and any rule local enough
 * to save the free end also saves this corbel — so composite vertical action was adopted, and
 * this test is one of the things it makes wrong. The wall over a raking cut now resists as a deep
 * beam — a vertical section through eleven courses of bonded masonry, 11,627 cm3 against one bed
 * patch's 179.48 — and every rung of the ladder is re-derived from that section in
 * StaircaseWallTestSupport.h rather than from whatever makes this file green.
 *
 * What is unchanged is the whole load path, asserted here exactly as it was: the same eleven
 * forces, 38.5 brick weights down to 1, to the same 1e-5. Composite action changes the section
 * the moment is read against and nothing about what the wall hands down, so a change that moved
 * the ladder itself fails on the force rows first.
 *
 * WHY A SEPARATE TEST FROM THE INTEGRATION ONE. That test cuts the same void through a real wall
 * in a real world and watches the bricks come down; it used to assert the corbel's utilisation as
 * the precondition that made "the overhang fell" mean something, and can no longer read it — the
 * cascade now runs inside the commit, a given joint carries exactly nothing, and only the
 * breaking call reports the ratio that broke it (DESIGN.md §3). By the time a world test can
 * look, the ladder reads zero all the way up. SolveLoads is non-destructive by contract
 * ("solving must leave every connection exactly as intact as it found it"), so the same void cut
 * into the same wall can be read at full precision here with the joints still standing after —
 * this is where the eleven numbers live; the integration test keeps the half only a world can
 * answer, that those five joints then actually gave and the bricks then actually fell.
 *
 * THE ORACLE IS HAND ARITHMETIC, NOT A SECOND SOLVER. StaircaseWallTestSupport works two ladders
 * out from the picture — the force, where each corbelled brick takes its own weight, all of the
 * corbelled brick above it and half of the next brick along; and the moment, where each step adds
 * its own 5.625 cm arm to the step above's moment carried across the 11.25 cm the corbel has
 * stepped out — then turns the pair into a stress with beam theory on the 10.25 x 10.25 patch it
 * hangs off. Nothing in either derivation walks the graph, so agreeing with them is evidence
 * rather than tautology.
 *
 * WHICH AXIS GOVERNS IS ASSERTED BEFORE ANYTHING IS CLAIMED. ComputeUtilisation returns the worst
 * of compression, shear and tension, so a fixture aimed at bending would silently measure
 * compression the moment compression is higher. Relieving the opened edge closes a gap that used
 * to be a factor of 92; the squeezed edge is relieved with it, and the bottom rung now reads
 * 0.0527 in tension against 0.00977 in compression, a 5.4x margin. Shear is exactly zero, since
 * gravity is normal to a bed joint.
 *
 * TWO CONTROLS: the wall as built must have nothing over capacity, or the staircase caused none
 * of this; and no surviving piece may be Stranded, or a wall reported as unheld would be the
 * solver declining to divide load round a loop rather than physics.
 *
 * Needs a ticking world: no. Not one line needs an actor, a tick or a renderer — it is boxes and
 * doubles, which is why the magnitudes belong here and the composition belongs in
 * Integration.AStaircaseVoidLeavesTheOverhangStanding.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStaircaseCorbelLoadTest,
	"DestructionGame.Core.Structure.AStaircaseVoidCondemnsTheCorbel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStaircaseCorbelLoadTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace StaircaseWallTestSupport;

	/* The expected numbers are ratios of published strengths, so they mean what they say only
	 * while the profile still carries the figures they were derived against — asserted rather
	 * than imported, since a test that read the profile would agree with a wrong profile. */
	TestTrue(
		*FString::Printf(TEXT("fixture: derived against f_xk1 = %g MPa, the profile carries %g"),
			StaircaseMortarTensileMPa, GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == StaircaseMortarTensileMPa);

	TestTrue(
		*FString::Printf(TEXT("fixture: derived against compressive %g MPa, the profile carries %g"),
			StaircaseMortarCompressiveMPa, GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == StaircaseMortarCompressiveMPa);

	TestTrue(
		*FString::Printf(TEXT("fixture: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == 1.9);

	/* And the axis: every rung is compared here, not only the worst one, because the ladder spans
	 * a factor of 38 in load and both stresses are linear in it — so if tension governs at one
	 * end it governs at the other, and saying so for all eleven costs one loop. */
	for (int32 Course = StaircaseLowestCorbelCourse; Course <= StaircaseHighestCorbelCourse; ++Course)
	{
		const double Tension = StaircasePredictedCorbelUtilisation(Course);
		const double Compression = StaircasePredictedCorbelCompressionUtilisation(Course);

		TestTrue(
			*FString::Printf(
				TEXT("fixture precondition: bending in TENSION must govern course %d's corbel, or this measures the wrong axis — tension %.8f against compression %.8f"),
				Course, Tension, Compression),
			Tension > Compression);
	}

	FBrickLayout Laid;

	if (!RunningBond(StaircaseWallSpec(), Laid) || Laid.Boxes.Num() != StaircaseWallPieceCount)
	{
		AddError(FString::Printf(
			TEXT("fixture: a flush 13 x 10 wall should lay as %d pieces, got %d"),
			StaircaseWallPieceCount, Laid.Boxes.Num()));

		return true;
	}

	TestTrue(
		TEXT("fixture: the laid wall must know where every piece and every joint is, or every moment below is silently zero"),
		Laid.Structure.HasCompleteGeometry());

	/* The positive control: a wall that arrived with a joint past capacity would be condemned for
	 * a reason the staircase had nothing to do with, and the flush end is what buys this — see
	 * StaircaseWallSpec for why a ragged 13-course wall reads 0.36 at its own ends untouched. */
	Laid.Structure.SolveLoads();

	double WorstAsBuilt = 0.0;
	int32 WorstAsBuiltJoint = INDEX_NONE;

	for (int32 Joint = 0; Joint < Laid.Structure.NumConnections(); ++Joint)
	{
		const double Utilisation = Laid.Structure.GetConnectionUtilisation(Joint);

		if (Utilisation > WorstAsBuilt)
		{
			WorstAsBuilt = Utilisation;
			WorstAsBuiltJoint = Joint;
		}
	}

	AddInfo(FString::Printf(
		TEXT("as built, the worst of %d joints is %d at %.8f of capacity"),
		Laid.Structure.NumConnections(), WorstAsBuiltJoint, WorstAsBuilt));

	TestTrue(
		*FString::Printf(
			TEXT("the wall as built must have nothing over capacity, joint %d reads %.8f"),
			WorstAsBuiltJoint, WorstAsBuilt),
		WorstAsBuilt < 1.0);

	/* THE CUT — the same 36 bricks, in the same stepped diagonal, as the player's own click takes. */
	const TArray<int32> VoidPieces = StaircaseVoidPieces(Laid.Boxes);

	if (VoidPieces.Num() != StaircaseVoidPieceCount)
	{
		AddError(FString::Printf(TEXT("fixture: the staircase should cut %d bricks, it names %d"),
			StaircaseVoidPieceCount, VoidPieces.Num()));

		return true;
	}

	for (const int32 Piece : VoidPieces)
	{
		if (!Laid.Structure.RemovePiece(Piece))
		{
			AddError(FString::Printf(TEXT("fixture: piece %d should have been in the wall to remove"),
				Piece));

			return true;
		}
	}

	Laid.Structure.SolveLoads();

	/*
	 * THE LADDER, BOTTOM TO TOP, IN TWO SEPARATE CLAIMS. The force says the load routed the way
	 * the picture says it does — each step handing everything it carries to the one below — and
	 * the utilisation says the moment on that force was resolved correctly against the 10.25 cm
	 * patch it hangs off. Printing and asserting both is what makes a failure say which of the
	 * two is wrong, not only that the number moved.
	 *
	 * A RELATIVE TOLERANCE, measured rather than guessed. The ladder spans a factor of 38 in
	 * force and 286 in moment, so a fixed absolute slack would be meaningless at one end and
	 * vacuous at the other. Ten of the eleven force rungs agree with the hand arithmetic to every
	 * digit printed; the eleventh — the bottom step, integrating the most wall — comes back
	 * 2.4e-6 low, a real effect: the hand ladder assumes the load cone above each step lies
	 * entirely inside the wall, and the cone above the bottom step of a 10-brick-wide wall
	 * reaches X = 225 cm at course 12, where the wall stops at 213.75 (confirmed, not argued for,
	 * by widening the wall to 26 bricks per course, where the bottom rung matches the hand figure
	 * to the last digit — the fixture stays 10 wide because the integration test's geometry is
	 * written around that wall).
	 *
	 * That deficit no longer reaches the utilisation at all — a consequence of the section, not
	 * an improvement in the fixture. Under composite action the governing reading is pure bending
	 * on the vertical section, M / (t D^2 / 6), with no compression term for the deficit to share
	 * unevenly with, and the moment ladder is exact where the force ladder is 2.4e-6 low, since
	 * the missing brickwork is load the cone would have delivered straight down rather than out
	 * on an arm.
	 *
	 * So 1e-5: four times the worst measured deviation and five orders of magnitude tighter than
	 * any modelling error. A wrong lever arm, a wrong section modulus or a missing 100x would all
	 * be percent-scale or larger; nothing this test looks for can hide under a hundred-thousandth.
	 */
	constexpr double RelativeTolerance = 1.0e-5;

	int32 JointsOverCapacity = 0;
	double WorstCorbel = 0.0;

	for (int32 Course = StaircaseLowestCorbelCourse; Course <= StaircaseHighestCorbelCourse; ++Course)
	{
		const int32 Corbel = StaircaseCorbelPiece(Laid.Boxes, Course);
		const int32 Support = StaircaseCorbelSupportPiece(Laid.Boxes, Course);

		const int32 Joint = Corbel != INDEX_NONE && Support != INDEX_NONE
			? JointBetweenPieces(Laid.Structure, Corbel, Support)
			: INDEX_NONE;

		if (Joint == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("fixture: course %d should have a brick at X %.2f jointed to one at X %.2f; they are pieces %d and %d"),
				Course, StaircaseVoidEdgeXCm(Course),
				StaircaseVoidEdgeXCm(Course) + StaircaseHalfStepCm, Corbel, Support));

			continue;
		}

		const double ForceUu = Laid.Structure.GetConnectionForce(Joint).Size();
		const double Utilisation = Laid.Structure.GetConnectionUtilisation(Joint);

		const double ExpectedForceUu = StaircasePredictedCorbelForceUu(Course);
		const double ExpectedUtilisation = StaircasePredictedCorbelUtilisation(Course);

		AddInfo(FString::Printf(
			TEXT("corbel course %2d: piece %3d on piece %3d, joint %3d carries %11.4f uu (%5.2f W, expected %11.4f) and reads %.8f of capacity (expected %.8f)"),
			Course, Corbel, Support, Joint,
			ForceUu, ForceUu / StaircaseFullBrickWeightUu, ExpectedForceUu,
			Utilisation, ExpectedUtilisation));

		TestTrue(
			*FString::Printf(
				TEXT("course %d's corbel joint should carry %.4f uu (%.1f brick weights), it carries %.4f"),
				Course, ExpectedForceUu,
				StaircaseCorbelLoadBrickWeights[Course - StaircaseLowestCorbelCourse], ForceUu),
			FMath::Abs(ForceUu - ExpectedForceUu) <= RelativeTolerance * ExpectedForceUu);

		TestTrue(
			*FString::Printf(
				TEXT("course %d's corbel joint should read %.8f of capacity, it reads %.8f"),
				Course, ExpectedUtilisation, Utilisation),
			FMath::Abs(Utilisation - ExpectedUtilisation) <= RelativeTolerance * ExpectedUtilisation);

		/* And solving broke nothing, which is what makes this reading possible at all: if
		 * SolveLoads ever started breaking, every number above would go to zero and read as a
		 * wall carrying nothing rather than as a contract violation. */
		TestTrue(
			*FString::Printf(
				TEXT("solving is non-destructive, so course %d's corbel joint must still be intact after it"),
				Course),
			!Laid.Structure.GetConnection(Joint).HasGiven());

		WorstCorbel = FMath::Max(WorstCorbel, Utilisation);

		if (Utilisation > 1.0)
		{
			++JointsOverCapacity;
		}
	}

	/*
	 * Not one joint is over capacity, and the ladder is what makes that mean something: "nothing
	 * broke" is also true of a wall carrying nothing at all, so the eleven rungs are printed and
	 * asserted individually. They march 0.0083, 0.0223, 0.0247, 0.0279, 0.0313, 0.0347, 0.0383,
	 * 0.0419, 0.0454, 0.0490, 0.0527 — a real ladder, monotonic, six-fold top to bottom, each rung
	 * derived from the depth of masonry standing over it. A model that deleted the moment rather
	 * than re-sectioning it would read eleven zeroes and pass a count.
	 */
	AddInfo(FString::Printf(
		TEXT("the staircase leaves %d of %d corbel joints over capacity, worst %.8f (the arithmetic predicts %d and %.8f)"),
		JointsOverCapacity, StaircaseCorbelStepCount,
		WorstCorbel, StaircasePredictedCorbelJointsOverCapacity,
		StaircasePredictedWorstCorbelUtilisation));

	TestEqual(
		TEXT("the staircase must leave exactly the corbel joints the arithmetic condemns over capacity"),
		JointsOverCapacity, StaircasePredictedCorbelJointsOverCapacity);

	TestTrue(
		*FString::Printf(
			TEXT("the bottom of the corbel should read %.8f of capacity, it reads %.8f"),
			StaircasePredictedWorstCorbelUtilisation, WorstCorbel),
		FMath::Abs(WorstCorbel - StaircasePredictedWorstCorbelUtilisation)
			<= RelativeTolerance * StaircasePredictedWorstCorbelUtilisation);

	/* And no survivor is Stranded. A staircase void is exactly the shape that makes unroutable
	 * knots — cut two bricks in a row out from under a course and the pair above fall back on
	 * each other's head joints, each naming the other as its support — and a wall condemned
	 * because the solver declined to divide load round a loop is a model limitation wearing a
	 * collapse's clothes. The corbel is built from single bed joints for that reason: every step
	 * is statically determinate. */
	for (int32 Piece = 0; Piece < Laid.Structure.NumPieces(); ++Piece)
	{
		if (Laid.Structure.IsPieceRemoved(Piece))
		{
			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("piece %d must not be Stranded: that would make this a solver limitation rather than an overload"),
				Piece),
			Laid.Structure.GetPieceSupport(Piece) != EPieceSupport::Stranded);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
