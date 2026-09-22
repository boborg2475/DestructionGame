// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Layout.h"

/**
 * 3D shed builders: a closed box of four brick walls bracing each other at the corners (which a 2D
 * section cannot capture), a wood roof, and a door overhang on a wood post. Emits an FBrickLayout
 * flagged 3D (FStructure::SetThreeDimensional), adopted via AdoptLayout; the 3D bridge poses the
 * Y-normal corner joints. World-free: no UWorld or UObject, and nothing from Tests/.
 */
namespace DestructionShed3D
{
	/**
	 * The toy 3D shed: a coarse axis-aligned box so the LP stays tractable (THREED_DESIGN.md
	 * R-Scale) and collapses are hand-derivable. X width, Y depth, Z height. Each wall is one
	 * grounded block; side walls sit between back and front with a joint gap, so each corner is a
	 * Y-normal joint. Defaults are the canonical toy shed.
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

		// Timber roof beam, bearing on the back and front wall tops.

		/** Roof beam inset from each X edge so it clears the side walls, cm. */
		double RoofInsetXCm = 40.0;

		/** How far the roof laps onto each of the back and front wall tops along Y, cm. */
		double RoofBearingLapYCm = 10.0;

		/** Vertical thickness of the roof beam, cm. */
		double RoofThicknessCm = 12.0;

		// Timber overhang, screwed to the front wall, out over the door.

		/** X centre of the overhang beam and the post beneath it, cm. */
		double OverhangCentreXCm = 110.0;

		/** X footprint of the overhang beam (and the post), cm. */
		double OverhangWidthXCm = 20.0;

		/** Overhang length along Y, cm (it cantilevers in +Y over the door). */
		double OverhangLengthYCm = 200.0;

		/** Vertical thickness of the overhang beam, cm. */
		double OverhangThicknessCm = 12.0;

		/** Overhang lap onto the front wall top along Y (the tension fixing), cm. */
		double FixingLapYCm = 4.0;

		// Grounded Timber post under the front of the overhang.

		/** Y footprint of the post, cm. */
		double PostWidthYCm = 12.0;

		/**
		 * Post centre Y, cm. Outboard of the fixing, so the overhang's weight puts the fixing in
		 * tension; only post and fixing together hold it (R-Overhang).
		 */
		double PostCentreYCm = 280.0;
	};

	/** Lay the toy 3D shed (walls and post grounded) and flag it 3D. Writes nothing for an invalid spec. */
	bool Build(const FShed3DSpec& Spec, DestructionLayout::FBrickLayout& OutLayout);

	/**
	 * The recognizable 3D shed (v2): brick walls with door and window openings and Timber lintels,
	 * stepped gables, a Timber roof of purlins and a ridge, and a porch overhang on two posts. All
	 * axis-aligned, as the 3D bridge requires. Dimensions are builder constants the test pins.
	 * Returns false with an empty layout on refusal.
	 */
	bool BuildRecognizable(DestructionLayout::FBrickLayout& OutLayout);

	/**
	 * The same shed at real brick resolution: single-wythe running-bond walls of 21.5 x 10.25 x
	 * 6.5 cm bricks on 1 cm joints. Hundreds of blocks, above the LP gate's cap, so the router is
	 * the break authority. Dimensions are builder constants. Returns false with an empty layout on
	 * refusal.
	 */
	bool BuildRealistic(DestructionLayout::FBrickLayout& OutLayout);
}
