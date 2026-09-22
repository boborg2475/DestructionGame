// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Tests/StaircaseWallTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Cutting the staircase void puts the bottom of the corbel at 0.0527 of capacity, with none of
 * its eleven steps over (0.369 before the 2026-08-14 flexural bond change, 22.9 before the
 * 2026-08-06 composite-action ruling). The wall over a raking cut resists as a deep beam; each
 * rung is re-derived from that section in StaircaseWallTestSupport.h. The load path is
 * unchanged: eleven forces, 38.5 brick weights down to 1, to 1e-5.
 *
 * Separate from the integration test because a world test cannot read utilisation after the
 * cascade (given joints carry nothing). SolveLoads is non-destructive, so the numbers are read
 * here. The oracle is hand arithmetic (force and moment ladders, beam theory on the 10.25 x 10.25
 * patch), not a second solver.
 *
 * Tension is asserted to govern each rung first (bottom rung: 0.0527 tension vs 0.00977
 * compression); shear is zero on a bed joint. Controls: nothing over capacity as built, and no
 * Stranded survivor. No world needed.
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

	// The profile still carries the strengths the expectations were derived from.
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

	// Tension governs on every rung, not only the worst.
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

	// Control: nothing over capacity as built (see StaircaseWallSpec for why the ends are flush).
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

	// Cut the same 36 bricks the player's click removes.
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
	 * Force and utilisation are asserted separately per rung, so a failure says which is wrong.
	 * Relative tolerance 1e-5: the bottom force rung is 2.4e-6 low because its load cone runs past
	 * the wall's end (confirmed by widening the wall), while a wrong arm, section or 100x factor
	 * would be percent-scale.
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

		// SolveLoads is non-destructive; a broken joint would read as zero load.
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
	 * None over capacity; meaningful only because each rung was asserted above (0.0083 rising to
	 * 0.0527), since a model that dropped the moment would also pass a count.
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
	 * No survivor is Stranded: a staircase void can create head-joint loops, and a stranded piece
	 * would be a solver limitation, not an overload.
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
