// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Tests/StaircaseWallTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE STAIRCASE'S ARITHMETIC, WITH NO WORLD AND NOTHING BROKEN: CUTTING THE VOID PUTS THE BOTTOM
 * OF THE CORBEL AT 22.9 OF CAPACITY AND EIGHT OF ITS ELEVEN STEPS PAST THE LINE.
 *
 * WHY THIS EXISTS AS A SEPARATE TEST, AND IT IS NOT A DUPLICATE OF THE INTEGRATION ONE. The
 * integration test cuts the same void through a real wall in a real world and watches the bricks
 * come down. It used to assert the corbel's utilisation as the precondition that made "the overhang
 * fell" mean something — and it can no longer read it, for a reason that is the design working
 * rather than failing: the cascade now runs inside the commit, a joint that has given carries
 * exactly nothing, and only the BREAKING call reports the ratio that broke it (DESIGN.md §3). By
 * the time a test in a world can look, the ladder reads zero all the way up. Any wire that makes
 * the overhang fall must break those joints first, so the magnitudes are simply not observable
 * there.
 *
 * SolveLoads IS. It is non-destructive by contract — "solving must leave every connection exactly
 * as intact as it found it" — so the same void, cut into the same wall, can be read at full
 * precision here and the joints are still standing afterwards. This is where the eleven numbers
 * live; the integration test keeps the half that only a world can answer, which is that those five
 * joints then actually gave, in the first sweep, and that the bricks then actually fell.
 *
 * THE ORACLE IS HAND ARITHMETIC, NOT A SECOND SOLVER. StaircaseWallTestSupport works TWO ladders
 * out from the picture — the FORCE, where each corbelled brick takes its own weight, all of the
 * corbelled brick above it and half of the next brick along; and the MOMENT, where each step adds
 * its own 5.625 cm arm to the step above's moment carried across the 11.25 cm the corbel has
 * stepped out — and turns the pair into a stress with beam theory on the 10.25 x 10.25 patch it
 * hangs off. Nothing in either derivation walks the graph, so agreeing with them is evidence
 * rather than tautology.
 *
 * WHICH AXIS GOVERNS IS ASSERTED BEFORE ANYTHING IS CLAIMED. ComputeUtilisation returns the WORST
 * of compression, shear and tension, so a fixture aimed at bending silently measures compression
 * the moment compression is higher. Here the same load reads 22.93 in tension and 0.249 in
 * compression — a factor of 92 — and shear is exactly zero, because gravity is normal to a bed
 * joint. The comparison is spelled out rather than assumed, so a change that made compression
 * govern fails HERE instead of quietly measuring the wrong thing.
 *
 * TWO CONTROLS, AND NEITHER IS DECORATION. The wall as built must have nothing over capacity, or
 * the staircase caused none of this; and no surviving piece may be Stranded, or the wall came down
 * because the solver declined to divide load round a loop rather than because mortar gave.
 *
 * NEEDS A TICKING WORLD: NO. Not one line of this needs an actor, a tick or a renderer — it is
 * boxes and doubles, which is precisely why the magnitudes belong here and the composition belongs
 * in Integration.AStaircaseVoidBringsTheOverhangDown.
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

	/*
	 * THE EXPECTED NUMBERS ARE RATIOS OF PUBLISHED STRENGTHS, so they mean what they say only while
	 * the profile still carries the figures they were derived against. Asserted rather than
	 * imported: a test that read the profile would agree with a wrong profile.
	 */
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

	/*
	 * AND THE AXIS. Every rung is compared here, not only the worst one, because the ladder spans a
	 * factor of 38 in load and both stresses are linear in it — so if tension governs at one end it
	 * governs at the other, and saying so for all eleven costs one loop.
	 */
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

	/*
	 * THE POSITIVE CONTROL. A wall that arrived with a joint past capacity would be condemned for a
	 * reason the staircase had nothing to do with, and the flush end is what buys this — see
	 * StaircaseWallSpec for why a ragged 13-course wall reads 0.36 at its own ends before anyone
	 * has touched it.
	 */
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
	 * THE LADDER, BOTTOM TO TOP, IN TWO SEPARATE CLAIMS. The FORCE says the load routed the way the
	 * picture says it does — each step handing everything it carries to the one below — and the
	 * UTILISATION says the moment on that force was resolved correctly against the 10.25 cm patch
	 * it hangs off. Printing and asserting both is what makes a failure say WHICH of the two is
	 * wrong, rather than only that the number moved.
	 */
	/*
	 * A RELATIVE TOLERANCE, AND THE ONE THING IT HAS TO ADMIT IS MEASURED RATHER THAN GUESSED.
	 *
	 * The ladder spans a factor of 38 in force and 286 in moment, so a fixed absolute slack would be
	 * meaningless at one end and vacuous at the other. Ten of the eleven force rungs come back
	 * agreeing with the hand arithmetic to every digit printed. The eleventh — the bottom step, the
	 * one that integrates the most wall — comes back 2.4e-6 LOW, and that is a real effect rather
	 * than rounding: the hand ladder assumes the load cone above each step lies entirely inside the
	 * wall, and the cone above the BOTTOM step of a 10-brick-wide wall reaches X = 225 cm at course
	 * 12, where the wall stops at 213.75. The few brickless centimetres are load the idealisation
	 * counts and the wall does not have, worth 9.1e-5 of a brick weight out of 38.5.
	 *
	 * IT WAS CONFIRMED BY WIDENING THE WALL, NOT ARGUED FOR. Run identically at 26 bricks per
	 * course — wide enough that the cone clears the end — the bottom rung reads 102687.1471 uu,
	 * which is the hand figure to the last digit. The fixture stays 10 wide because the integration
	 * test spawns an actor per piece and its whole geometry is written around that wall; the
	 * idealisation is recorded here instead.
	 *
	 * THE SAME DEFICIT REACHES THE UTILISATION AS 1.0e-7, MEASURED: the bottom rung reads
	 * 22.92952820 against the hand ladder's 22.92952589. It lands SMALLER than the force's own 2.4e-6
	 * and on the other side, which is what a deficit shared unevenly between a bending term and the
	 * compression subtracted from it does; it is the same missing brickwork either way.
	 *
	 * SO 1e-5, WHICH IS FOUR TIMES THE WORST MEASURED DEVIATION AND FIVE ORDERS OF MAGNITUDE
	 * TIGHTER THAN ANY MODELLING ERROR. A wrong lever arm, a wrong section modulus or a missing
	 * 100x would all be percent-scale or larger; nothing this test is looking for can hide under a
	 * hundred-thousandth.
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

		/*
		 * AND SOLVING BROKE NOTHING, which is what makes this reading possible at all. If SolveLoads
		 * ever started breaking, every number above would go to zero and read as a wall carrying
		 * nothing rather than as a contract violation.
		 */
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
	 * EIGHT JOINTS ARE OVER CAPACITY, NOT ONE. The ladder crosses 1.0 between its eighth and ninth
	 * rungs, and that is the whole reason the eleven rungs are printed: a single joint over the line
	 * could be a fixture on a knife edge, and a ladder marching 0.058, 0.271, 0.722, 1.494, 2.672,
	 * 4.338, 6.577, 9.472, 13.107, 17.565, 22.930 cannot.
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

	/*
	 * AND NO SURVIVOR IS Stranded. A staircase void is exactly the shape that makes unroutable
	 * knots — cut two bricks in a row out from under a course and the pair above fall back on each
	 * other's head joints, each naming the other as its support — and a wall condemned because the
	 * solver declined to divide load round a loop is a model limitation wearing a collapse's
	 * clothes. The corbel is built from single bed joints for that reason: every step is
	 * statically determinate.
	 */
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
