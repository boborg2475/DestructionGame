// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/ConnectionStrength.h"

/**
 * The shared material profile library.
 *
 * The values and their citations are in MaterialProfiles.cpp.
 */
namespace DestructionProfiles
{
	/** A material: what a piece is made of, as opposed to what holds it on. */
	struct FMaterialProfile
	{
		/**
		 * Density in g/cm3 — Unreal's own unit for UPhysicalMaterial::Density, so
		 * published values go in unconverted. This is what lets a brick actor
		 * derive its mass from its dimensions instead of hand-setting 2.72 kg.
		 */
		double DensityGramsPerCubicCm = 0.0;

		/**
		 * The material's OWN directional strengths — the "brick crushes" failure
		 * mode, as distinct from "the mortar gives".
		 *
		 * Reuses FConnectionStrength so a material can be run through
		 * ComputeUtilisation unchanged rather than needing a parallel function.
		 */
		FConnectionStrength Strength;

		/**
		 * How well a connection bonds to this material's face. DECLARED AND
		 * DELIBERATELY UNUSED: connection-to-material pairing is deferred until a
		 * second material exists to prove it (see CURRENT_STATE.md). 1.0 means a
		 * connection reaches its full rated strength against this face.
		 */
		double BondFactor = 1.0;
	};

	/** One row of the library. Adding a material is adding one of these. */
	struct FNamedMaterialProfile
	{
		const TCHAR* Name = nullptr;
		FMaterialProfile Profile;
	};

	/** The calibration baseline: every other material is a ratio of this one. */
	extern const FMaterialProfile StructuralConcrete;

	extern const FMaterialProfile ClayBrick;

	/** Every material profile, so a sweep checks the whole library. */
	TArrayView<const FNamedMaterialProfile> AllMaterialProfiles();
}
