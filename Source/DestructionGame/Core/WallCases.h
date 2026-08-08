// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Layout.h"

/**
 * THE ACCEPTANCE-WALL PRODUCER — the third thing in this project that lays bricks, beside
 * DestructionLayout::RunningBond and DestructionCorbel::Build.
 *
 * WHY IT IS NOT RunningBond. Six of the twenty acceptance walls are not running-bond rectangles:
 * two corbel out, two carry a projecting header, and two are stack bond. RunningBond lays one
 * shape, so either there is a second producer or six of the configurations the user drew cannot be
 * laid at all — and the six include two of the matched pairs the acceptance set exists for.
 *
 * IT IS ONE RULE, NOT SIX SPECIAL CASES. Every course is described by two numbers — where its
 * right face is, and how long its rightmost piece is — and then laid RIGHT TO LEFT on the
 * coordinating pitch, closing with whatever is left at the wall's left face. Running bond, stack
 * bond, a corbel and a projecting header are all values of those two numbers. Laying right-to-left
 * is what keeps the cut closing piece at the LEFT end, away from every corbel and header under
 * test, rather than in the middle of what is being measured.
 *
 * IT EMITS AN FBrickLayout, so one of these walls goes into a world through the same
 * UDestructionStructureSubsystem::BuildLayout door a wall and a corbel already do.
 *
 * WORLD-FREE, like Core/Layout: boxes and doubles, no UWorld and no UObject.
 *
 * ONE DIRECTION OF INCLUSION. Nothing from Tests/ may be included here; a test may include this.
 *
 * THE ACCEPTANCE FIXTURE'S BRICKLAYER WAS COPIED, NOT MOVED, AND COMMIT 1c763c5 SAYS "MOVES". That
 * commit message is wrong and cannot be rewritten, so the correction lives here. `LayWall` in
 * Tests/WallAcceptanceTest.cpp still lays its own bricks and calls nothing in this namespace; the
 * commit is 853 insertions and ZERO deletions in that file. Two consequences are worth knowing.
 * First, it is why not one reading in the acceptance file moved across the commit — nothing it
 * measures went through here. Second, `Acceptance.Wall.TheProducerLaysTheWallTheFixtureLays` is
 * therefore STILL LIVE rather than tautological: it holds this producer against a genuinely
 * independent bricklayer, including handle order, and it is the only test that does. Do not delete
 * it, and do not "finish the move" by making the fixture call this — that would spend the one
 * comparison keeping the two honest.
 */
namespace DestructionWallCases
{
	/** How the courses line up. */
	enum class EWallBond : uint8
	{
		/** Alternate courses offset half a cell, half bats filling the ends flush. */
		Running,

		/** Every course identical, so every head joint lines up through the whole wall. */
		Stack,
	};

	/**
	 * A rectangle in (course, cell) space — the vocabulary a cut and a named outcome are written in.
	 *
	 * A piece is in the region when its course is within the inclusive course range and its cell
	 * index is STRICTLY between the two cell bounds.
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
		/** FULL brick dimensions, cm. UK metric standard is 21.5 x 10.25 x 6.5. */
		FVector BrickSizeCm = FVector(21.5, 10.25, 6.5);

		double JointThicknessCm = 1.0;

		/** g/cm3, Unreal's own unit for density — published values go in unconverted. */
		double DensityGramsPerCubicCm = 0.0;

		int32 CoursesHigh = 0;

		/** Full bricks in an even course. */
		int32 Cells = 0;

		EWallBond Bond = EWallBond::Running;

		/** First course that steps out, or INDEX_NONE. Every course from it steps out again. */
		int32 CorbelFromCourse = INDEX_NONE;
		double CorbelStepCm = 0.0;

		/**
		 * A course whose end brick is pushed half a cell past the face of the wall, or INDEX_NONE.
		 *
		 * A flush odd course normally closes with a half bat; pushing the face out half a cell and
		 * closing with a FULL brick instead puts a whole brick where the bat was, half of it bearing
		 * on the course below and half of it over air.
		 */
		int32 ProjectingCourse = INDEX_NONE;

		/** The joint profile every connection in the wall is built with. */
		FConnectionStrength Strength;
	};

	/**
	 * A laid acceptance wall: the graph and the boxes whoever spawns actors wants, plus where every
	 * piece sits on the coordinating grid.
	 *
	 * The three arrays are parallel to the structure's piece array, indexed by the same handles.
	 */
	struct FWallLayout
	{
		DestructionLayout::FBrickLayout Layout;

		TArray<int32> CourseOf;
		TArray<double> CellOf;
	};

	/**
	 * Lay one, bottom course grounded.
	 *
	 * Refuses a spec that could not describe a wall, writing nothing.
	 *
	 * @return true if a wall was laid.
	 */
	bool Build(const FWallSpec& Spec, FWallLayout& OutWall);

	/** Every live piece the regions name, in handle order. */
	void PiecesInRegions(
		const FWallLayout& Wall,
		TArrayView<const FWallRegion> Regions,
		TArray<int32>& OutPieces);
}
