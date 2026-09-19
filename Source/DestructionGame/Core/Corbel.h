// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Layout.h"

/**
 * THE STEPPED CORBEL PRODUCER — the second thing in this project that lays bricks, beside
 * DestructionLayout::RunningBond.
 *
 * It emits an FBrickLayout and nothing else, so a corbel goes into a world through the same
 * UDestructionStructureSubsystem::BuildLayout door a wall does — a second spawn loop is a second
 * answer to where a brick goes and what it weighs, and this project has paid for two before.
 *
 * World-free, like Core/Layout: boxes and doubles, no UWorld and no UObject. One direction of
 * inclusion — nothing from Tests/ may be included here; a test may include this.
 */
namespace DestructionCorbel
{
	/** One structure in the corbel family, as data. */
	struct FCorbelSpec
	{
		/** Every length is multiplied by this. */
		double Scale = 1.0;

		/** How far the arm's outer face advances per course, cm, before scaling. */
		double StepCm = 11.25;

		/** Courses of immovable base. Odd, so the top course of the base is unshifted. */
		int32 BaseCourses = 3;

		/** Cells wide the base is. */
		int32 BaseCells = 2;

		/** Stepped courses standing on the base. */
		int32 Steps = 10;

		/** Left-hand edge of the base, cm, before scaling. */
		double LeftOriginCm = 0.0;

		/**
		 * Whether each stepped course is filled inboard of its outer brick.
		 *
		 * False is the BARE ARM — one brick per course, a different load path rather than a
		 * thinner version of the same one.
		 */
		bool bFilled = true;

		FConnectionStrength Strength;
	};

	/**
	 * Lay one, base grounded.
	 *
	 * Refuses a spec that could not describe a structure, writing nothing.
	 *
	 * @return true if a corbel was laid.
	 */
	bool Build(const FCorbelSpec& Spec, DestructionLayout::FBrickLayout& OutLayout);
}
