// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Layout.h"

/**
 * Producer for the acceptance walls, alongside RunningBond and DestructionCorbel::Build. Needed
 * because six of the twenty walls are not running-bond rectangles (corbels, projecting headers,
 * stack bond).
 *
 * One rule: each course is described by its right face and its rightmost piece length, then laid
 * right to left on the pitch, so the cut closing piece lands at the left end, away from any corbel
 * or header under test.
 *
 * Emits an FBrickLayout for UDestructionStructureSubsystem::BuildLayout. World-free; must not
 * include anything from Tests/.
 *
 * Tests/WallAcceptanceTest.cpp's LayWall is an independent copy (commit 1c763c5 says "moved"; it
 * was copied). Keep it independent: Acceptance.Wall.TheProducerLaysTheWallTheFixtureLays compares
 * the two, including handle order.
 */
namespace DestructionWallCases
{
	/** How the courses line up. */
	enum class EWallBond : uint8
	{
		/** Alternate courses offset half a cell, half bats filling the ends flush. */
		Running,

		/** Every course identical; head joints line up through the wall. */
		Stack,
	};

	/**
	 * A rectangle in (course, cell) space, used for cuts and outcomes. Course range is inclusive;
	 * cell bounds are strict.
	 */
	struct FWallRegion
	{
		int32 CourseLo = 0;
		int32 CourseHi = 0;
		double CellLo = 0.0;
		double CellHi = 0.0;
	};

	/** One acceptance wall's geometry, as data. */
	struct FWallSpec
	{
		/** Full brick dimensions, cm (UK metric standard). */
		FVector BrickSizeCm = FVector(21.5, 10.25, 6.5);

		double JointThicknessCm = 1.0;

		/** g/cm3; published values go in unconverted. */
		double DensityGramsPerCubicCm = 0.0;

		int32 CoursesHigh = 0;

		/** Full bricks in an even course. */
		int32 Cells = 0;

		EWallBond Bond = EWallBond::Running;

		/** First course that steps out, or INDEX_NONE. Every course from it steps out again. */
		int32 CorbelFromCourse = INDEX_NONE;
		double CorbelStepCm = 0.0;

		/**
		 * A course whose end is a full brick pushed half a cell past the wall face (instead of a
		 * half bat), half over air, or INDEX_NONE.
		 */
		int32 ProjectingCourse = INDEX_NONE;

		/** Joint profile for every connection. */
		FConnectionStrength Strength;
	};

	/** A laid wall, plus each piece's course and cell; arrays are indexed by piece handle. */
	struct FWallLayout
	{
		DestructionLayout::FBrickLayout Layout;

		TArray<int32> CourseOf;
		TArray<double> CellOf;
	};

	/** Lay a wall, bottom course grounded. Returns false, writing nothing, for an invalid spec. */
	bool Build(const FWallSpec& Spec, FWallLayout& OutWall);

	/** Every live piece the regions name, in handle order. */
	void PiecesInRegions(
		const FWallLayout& Wall,
		TArrayView<const FWallRegion> Regions,
		TArray<int32>& OutPieces);
}
