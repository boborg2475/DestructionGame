// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Layout.h"

/**
 * THE SHED BUILDER — SHED_PATH.md Phase F, slice F1: the first authored, catalogue-buildable
 * shed, and the third thing in this project that lays a structure beside DestructionLayout::
 * RunningBond and DestructionCorbel::Build.
 *
 * IT EMITS AN FBrickLayout AND NOTHING ELSE, so a shed reaches a world through the same
 * AdoptLayout / BuildLayout door a wall and a corbel do — and, because F0 now carries per-piece
 * MATERIAL through AdoptLayout, the shed's cross-material physics (wood roof on brick heads, wood
 * post under wood overhang, screw fixing in tension) survives into play rather than going inert.
 *
 * WORLD-FREE, like Core/Layout and Core/Corbel: boxes and doubles, no UWorld and no UObject. One
 * direction of inclusion — a test may include this; nothing from Tests/ may be included here.
 *
 * ===========================================================================================
 * F1 COMPILE STUB. Written by test-expert so the Phase F1 red compiles and runs. Build() here is
 * a bare `return false` that lays NOTHING — no pieces, no joints. The failing test in
 * Tests/ShedBuilderTest.cpp is therefore RED because the shed is not built yet, exactly as the
 * corbel and running-bond producers were stubbed before their reds drove them. dev-expert (F1)
 * implements Build() to lay the cross-section the test's header spells out. NO LOGIC belongs in
 * the stub — the arithmetic that positions the pieces and forms the joints is the green step.
 * ===========================================================================================
 */
namespace DestructionShed
{
	/**
	 * The shed cross-section, as data — a 2D X-Z slice through the doorway, front-to-back.
	 *
	 * X IS THE DEPTH AXIS (back pier toward front pier toward the door, increasing X); Z is height;
	 * Y is the single wythe. The camera is mirrored along Y in play, which leaves an X-Z section
	 * reading the right way round — the authoring convention Tests/ShedBuilderTest.cpp derives from.
	 *
	 * A MASONRY PIER IS TWO PIECES, NOT FORTY COURSES: a grounded BASE (several real courses fused
	 * into one immovable block) plus one removable HEAD course. That keeps the cross-section a
	 * handful of pieces — far below the 200-block cap, so the equilibrium LP is the break authority
	 * — while still giving a course to pull. Full-resolution walls are a later, coarser-block slice.
	 *
	 * Defaults describe the canonical shed the F1 test builds and the F1 scenario row will publish.
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
		 * Front (largest-X) edge of the roof beam, cm. Its back edge is the back pier's own left
		 * edge, so it laps the back head fully and the front head as far as this — which must land
		 * INSIDE the front pier so the roof bears on it, and short of the front pier's front face
		 * so the overhang's fixing lap has the rest of that head to itself.
		 */
		double RoofFrontCm = 190.0;

		/* --- the wooden overhang (Timber), screwed to the front head, out over the door -------- */

		/**
		 * Back (smallest-X) edge of the overhang, cm. It laps the FRONT face of the front head from
		 * here to the head's front edge — that lap is the tension fixing patch — then cantilevers
		 * forward over the door. A small lap is the whole trick: too little lever for the screw to
		 * cantilever the beam unaided, so pulling the post drops it.
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
		 * X of the post's centre, cm — OUTBOARD of the fixing so the overhang's weight wants to
		 * rotate about the post with its back end lifting, forcing the fixing into TENSION. Neither
		 * the post alone nor the fixing alone holds it; both together do (R-Overhang).
		 */
		double PostCentreCm = 260.0;
	};

	/**
	 * Lay the shed, both piers and the post grounded.
	 *
	 * Refuses a spec that could not describe a shed, writing nothing.
	 *
	 * @return true if a shed was laid.
	 */
	bool Build(const FShedSpec& Spec, DestructionLayout::FBrickLayout& OutLayout);
}
