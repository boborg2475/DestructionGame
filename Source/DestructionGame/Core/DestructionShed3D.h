// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Layout.h"

/**
 * THE 3D SHED BUILDER — SHED_PATH.md Phase F on top of THREED_DESIGN.md E1-E3: the first
 * GENUINELY-THREE-DIMENSIONAL authored shed, and the payoff of the 3D LP work. Where
 * DestructionShed::Build lays a flat X-Z CROSS-SECTION through the doorway, this lays a CLOSED
 * BOX — four brick walls in two orthogonal planes that brace each other at the corners, the one
 * structural fact a 2D section can never capture — a wood roof bearing on the wall heads, and the
 * front-door overhang on a wood post (the C2 mechanism, now posed out of the X-Z plane).
 *
 * IT EMITS AN FBrickLayout AND FLAGS THE STRUCTURE 3D (FStructure::SetThreeDimensional), so a shed
 * reaches a world and the rigid-block LP through the same AdoptLayout door a wall and the 2D shed
 * do — and the bridge (E3) poses its out-of-plane (Y-normal) corner joints to the 3D LP rather than
 * refusing them.
 *
 * WORLD-FREE, like Core/Layout and Core/DestructionShed: boxes and doubles, no UWorld and no
 * UObject. One direction of inclusion — a test may include this; nothing from Tests/ may be
 * included here.
 */
namespace DestructionShed3D
{
	/**
	 * The toy 3D shed, as data — a closed axis-aligned box, kept COARSE (a handful of blocks) so the
	 * 3D LP stays in the tractable few-dozen-block band (THREED_DESIGN.md R-Scale) and every collapse
	 * is hand-derivable.
	 *
	 * X is width, Y is depth (into the door), Z is height. Every wall is ONE grounded block, not a
	 * course stack: four single blocks close the box and let the corner joints — the walls' shared,
	 * out-of-plane (Y-facing) faces — be laid without a base/head split. Full-resolution walls are a
	 * later, coarser-block slice (E4).
	 *
	 * Defaults describe the canonical toy shed the test builds. The side (left/right) walls fit
	 * BETWEEN the back and front walls with a JointThicknessCm gap on the Y axis, so each corner is a
	 * genuine face-sharing joint whose normal points along +/-Y.
	 */
	struct FShed3DSpec
	{
		/** X/Y footprint of every wall (thin dimension), cm. One wythe. */
		double WallThicknessCm = 20.0;

		/** Height of every grounded wall block, cm. */
		double WallHeightCm = 180.0;

		/** The mortar / bearing gap every joint is formed across, cm. */
		double JointThicknessCm = 1.0;

		/** Outer box footprint along X (back and front walls span [0, this]), cm. */
		double BoxWidthXCm = 220.0;

		/** Outer box footprint along Y (front wall's outer face is at this Y), cm. */
		double BoxDepthYCm = 220.0;

		/* --- the wooden roof beam (Timber), bearing on the back and front wall tops ------------- */

		/** How far the roof beam is inset from each X edge of the box, cm (so it clears the sides). */
		double RoofInsetXCm = 40.0;

		/** How far the roof laps onto each of the back and front wall tops along Y, cm. */
		double RoofBearingLapYCm = 10.0;

		/** Vertical thickness of the roof beam, cm. */
		double RoofThicknessCm = 12.0;

		/* --- the wooden overhang (Timber), screwed to the front wall, out over the door --------- */

		/** X centre of the overhang beam and the post beneath it, cm. */
		double OverhangCentreXCm = 110.0;

		/** X footprint of the overhang beam (and the post), cm. */
		double OverhangWidthXCm = 20.0;

		/** Overhang length along Y, cm (it cantilevers in +Y over the door). */
		double OverhangLengthYCm = 200.0;

		/** Vertical thickness of the overhang beam, cm. */
		double OverhangThicknessCm = 12.0;

		/** How far the overhang laps the front wall top along Y — the tension fixing patch, cm. */
		double FixingLapYCm = 4.0;

		/* --- the grounded wooden post (Timber) under the front of the overhang ------------------ */

		/** Y footprint of the post, cm. */
		double PostWidthYCm = 12.0;

		/**
		 * Y of the post's centre, cm — OUTBOARD of the fixing so the overhang's weight wants to
		 * rotate about the post with its back end lifting, forcing the fixing into TENSION. Neither
		 * the post alone nor the fixing alone holds it; both together do (R-Overhang), now in 3D.
		 */
		double PostCentreYCm = 280.0;
	};

	/**
	 * Lay the toy 3D shed, all four walls and the post grounded, and flag the structure 3D.
	 *
	 * Refuses a spec that could not describe a shed, writing nothing.
	 *
	 * @return true if a shed was laid.
	 */
	bool Build(const FShed3DSpec& Spec, DestructionLayout::FBrickLayout& OutLayout);
}
