// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/BuildMode/DemoBuilding.h"
#include "Core/Profiles/MaterialProfiles.h"

namespace BuildMode
{
	/*
	 * A PROOF-OF-CONCEPT SCRIPTED BUILD. Nothing here is special to a "demo": it is an
	 * ordinary sequence of PlacePiece calls, the exact same call the interactive render
	 * and a later build-mode UI drive one piece at a time. The table below is only a
	 * convenient way to list the sequence; a player pointing and clicking produces the
	 * same placements.
	 *
	 * The wall is a two-course running-bond course (grounded course 0, staggered course
	 * 1) with a Timber wall-plate bearing across the top. Every joint is X/Z-normal, so
	 * the structure is planar and needs no SetThreeDimensional before SolveLoads.
	 */
	TArray<FPlacementResult> BuildDemoBuilding(
		DestructionLayout::FBrickLayout& OutLayout,
		const FSnapSettings& Settings)
	{
		using namespace DestructionProfiles;

		struct FStep
		{
			FVector RequestedCentreCm;
			bool bGrounded;
			const FMaterialProfile* Material;
			FVector ExtentCm;
		};

		const FVector HalfBrick(10.75, 5.125, 3.25);

		const FStep Steps[] = {
			// Course 0: a grounded run of four bricks laid end to end.
			{ FVector(0.0,   0.0, 0.0),   true,  &ClayBrick, HalfBrick },
			{ FVector(22.5,  0.0, 0.0),   true,  &ClayBrick, HalfBrick },
			{ FVector(45.0,  0.0, 0.0),   true,  &ClayBrick, HalfBrick },
			{ FVector(67.5,  0.0, 0.0),   true,  &ClayBrick, HalfBrick },
			// Course 1: three bricks staggered a half-brick, bedding onto course 0.
			{ FVector(11.25, 0.0, 7.5),   false, &ClayBrick, HalfBrick },
			{ FVector(33.75, 0.0, 7.5),   false, &ClayBrick, HalfBrick },
			{ FVector(56.25, 0.0, 7.5),   false, &ClayBrick, HalfBrick },
			// A Timber wall-plate spanning course 1, bearing on all three of its bricks.
			{ FVector(33.75, 0.0, 16.75), false, &Timber,    FVector(33.75, 5.125, 5.0) },
		};

		TArray<FPlacementResult> Results;
		Results.Reserve(UE_ARRAY_COUNT(Steps));
		for (const FStep& Step : Steps)
		{
			Results.Add(PlacePiece(
				OutLayout,
				Step.RequestedCentreCm,
				Step.ExtentCm,
				*Step.Material,
				Step.bGrounded,
				Settings));
		}
		return Results;
	}
}
