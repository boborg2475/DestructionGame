// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/ConnectionStrength.h"

/** The shared material profile library. Values and citations are in MaterialProfiles.cpp. */
namespace DestructionProfiles
{
	/** A material: what a piece is made of, as opposed to what holds it on. */
	struct FMaterialProfile
	{
		/** Density in g/cm3, Unreal's own unit, so published values go in unconverted. */
		double DensityGramsPerCubicCm = 0.0;

		/**
		 * The material's own strengths ("the brick crushes", not "the mortar gives"). An
		 * FConnectionStrength so ComputeUtilisation applies unchanged.
		 */
		FConnectionStrength Strength;

		/**
		 * How well a connection bonds to this face. Below 1.0 derates the bond (tension, shear
		 * cohesion), never bearing. Read by EffectiveBondedStrength (SHED_PATH.md B2). Every
		 * shipped material is 1.0 until there is evidence for less.
		 */
		double BondFactor = 1.0;

		/**
		 * Whether tensile strength is a small fraction of crushing strength (masonry, concrete;
		 * not wood or steel). The library sweep's "compressive >= 5x tensile" check keys off it.
		 */
		bool bCompressionDominant = true;
	};

	/**
	 * One row of the library. The profile is a reference, never a copy: pieces store the
	 * shipped constant's address, and lookup compares addresses, so a copy would match nothing.
	 */
	struct FNamedMaterialProfile
	{
		const TCHAR* Name = nullptr;
		const FMaterialProfile& Profile;
	};

	/** The calibration baseline: every other material is a ratio of this one. */
	extern const FMaterialProfile StructuralConcrete;

	extern const FMaterialProfile ClayBrick;

	extern const FMaterialProfile Timber;

	/** Every material profile, so a sweep checks the whole library. */
	TArrayView<const FNamedMaterialProfile> AllMaterialProfiles();
}

namespace DestructionForce
{
	/*
	 * The connection x material weakest-link pairing (SHED_PATH.md B2), per axis:
	 *   tension, cohesion: min(ConnectionCap x min(BondFactor_A, BondFactor_B), MatCap_A, MatCap_B)
	 *   compression:       min(ConnectionCap, MatCap_A, MatCap_B)
	 * Friction and the shear ceiling come from the connection unchanged.
	 */
	FConnectionStrength EffectiveBondedStrength(
		const FConnectionStrength& Connection,
		const DestructionProfiles::FMaterialProfile& FaceA,
		const DestructionProfiles::FMaterialProfile& FaceB);
}
