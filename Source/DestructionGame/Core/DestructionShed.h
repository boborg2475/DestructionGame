// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Layout.h"

/**
 * The shed builder, a producer alongside RunningBond and DestructionCorbel::Build. Emits an
 * FBrickLayout with per-piece materials (brick piers, timber roof, overhang and post).
 * World-free.
 */
namespace DestructionShed
{
	/**
	 * The shed as a 2D X-Z section through the doorway (X depth, Z height, Y one wythe). Each
	 * pier is a grounded base plus one removable head course, keeping the section under the
	 * 200-block cap so the equilibrium LP is the break authority. Defaults are the canonical shed.
	 */
	struct FShedSpec
	{
		/** Y depth of every piece, cm. */
		double WytheCm = 20.0;

		/** Joint thickness, cm. */
		double JointThicknessCm = 1.0;

		// The two ClayBrick piers.

		/** X footprint of each pier, cm. */
		double PierWidthCm = 40.0;

		/** Height of the grounded base block, cm. */
		double BaseHeightCm = 170.0;

		/** Height of the removable head course, cm. */
		double HeadHeightCm = 10.0;

		/** Back (smallest-X) edge of the back pier, cm. */
		double BackPierLeftCm = 0.0;

		/** Distance between the piers' back edges, cm: the shed's depth. */
		double PierSeparationCm = 160.0;

		// The Timber roof beam, bearing on both heads.

		/** Vertical thickness of the roof beam, cm. */
		double RoofThicknessCm = 12.0;

		/**
		 * Front edge of the roof beam, cm (it starts at the back pier's back edge). Must land
		 * inside the front pier, short of its front face, to leave the overhang room to lap.
		 */
		double RoofFrontCm = 190.0;

		// The Timber overhang, screwed to the front head and cantilevered over the door.

		/**
		 * Back edge of the overhang, cm; it laps the front head from here (the fixing patch). The
		 * lap is deliberately too short for the screw alone, so pulling the post drops it.
		 */
		double OverhangBackCm = 196.0;

		/** Overhang length along X, cm. */
		double OverhangLengthCm = 200.0;

		/** Vertical thickness of the overhang beam, cm. */
		double OverhangThicknessCm = 12.0;

		// The grounded Timber post under the overhang.

		/** X footprint of the post, cm. */
		double PostWidthCm = 12.0;

		/**
		 * X of the post's centre, cm. Outboard of the fixing, so the overhang loads the fixing in
		 * tension; it needs both post and fixing to stand (R-Overhang).
		 */
		double PostCentreCm = 260.0;
	};

	/** Lay the shed with piers and post grounded. Refuses an invalid spec, writing nothing. */
	bool Build(const FShedSpec& Spec, DestructionLayout::FBrickLayout& OutLayout);
}
