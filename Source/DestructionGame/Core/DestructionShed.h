// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Layout.h"

/**
 * The shed builder: an authored, catalogue-buildable shed, third producer alongside
 * DestructionLayout::RunningBond and DestructionCorbel::Build.
 *
 * Emits an FBrickLayout, so a shed reaches a world through the same AdoptLayout/BuildLayout door a
 * wall and a corbel do. Per-piece material survives AdoptLayout, so the cross-material physics
 * (wood roof on brick heads, wood post under wood overhang, screw fixing in tension) plays out.
 *
 * World-free: boxes and doubles, no UWorld or UObject. Tests may include this; not the reverse.
 */
namespace DestructionShed
{
	/**
	 * The shed cross-section, as data: a 2D X-Z slice through the doorway. X is depth (back pier to
	 * front pier to door), Z is height, Y is the single wythe.
	 *
	 * A pier is two pieces, not forty courses: a grounded base plus one removable head course. That
	 * keeps the section under the 200-block cap, so the equilibrium LP is the break authority, while
	 * still leaving a course to pull. Defaults are the canonical shed the tests build.
	 */
	struct FShedSpec
	{
		/** Y depth of every piece, cm. One wythe, so every bed joint's Y overlap is full. */
		double WytheCm = 20.0;

		/** The mortar / bearing contact every joint is formed across, cm. */
		double JointThicknessCm = 1.0;

		/* --- the two masonry piers (ClayBrick) ------------------------------------------------ */

		/** X footprint of each pier, cm. */
		double PierWidthCm = 40.0;

		/** Height of the grounded base block, cm (the fused lower courses). */
		double BaseHeightCm = 170.0;

		/** Height of the single removable head course, cm. */
		double HeadHeightCm = 10.0;

		/** Left (smallest-X, back) edge of the BACK pier, cm. */
		double BackPierLeftCm = 0.0;

		/** Left edge of the FRONT pier minus that of the back pier, cm — the shed's depth. */
		double PierSeparationCm = 160.0;

		/* --- the wooden roof beam (Timber), bearing on both heads ------------------------------ */

		/** Vertical thickness of the roof beam, cm. */
		double RoofThicknessCm = 12.0;

		/**
		 * Front (largest-X) edge of the roof beam, cm. Its back edge is the back pier's left edge,
		 * so it laps the back head fully and the front head to here — which must land inside the
		 * front pier (so the roof bears) and short of its front face (leaving the overhang lap room).
		 */
		double RoofFrontCm = 190.0;

		/* --- the wooden overhang (Timber), screwed to the front head, out over the door -------- */

		/**
		 * Back (smallest-X) edge of the overhang, cm. It laps the front head from here to the head's
		 * front edge (the tension fixing patch), then cantilevers over the door. The lap is small on
		 * purpose: too little lever for the screw alone, so pulling the post drops it.
		 */
		double OverhangBackCm = 196.0;

		/** Overhang length along X, cm. */
		double OverhangLengthCm = 200.0;

		/** Vertical thickness of the overhang beam, cm. */
		double OverhangThicknessCm = 12.0;

		/* --- the grounded wooden post (Timber) under the front of the overhang ----------------- */

		/** X footprint of the post, cm. */
		double PostWidthCm = 12.0;

		/**
		 * X of the post's centre, cm. Outboard of the fixing, so the overhang's weight rotates the
		 * post and forces the fixing into tension. Neither post nor fixing alone holds it; both do
		 * (R-Overhang).
		 */
		double PostCentreCm = 260.0;
	};

	/**
	 * Lay the shed, both piers and the post grounded. Refuses a spec that could not describe a shed,
	 * writing nothing.
	 *
	 * @return true if a shed was laid.
	 */
	bool Build(const FShedSpec& Spec, DestructionLayout::FBrickLayout& OutLayout);
}
