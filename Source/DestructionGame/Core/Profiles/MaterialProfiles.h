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
		 * How well a connection bonds to this material's face. 1.0 means a connection
		 * reaches its full rated strength against this face; below 1.0 derates the
		 * BOND (tension and shear cohesion) but never the bearing (compression). Read
		 * by `EffectiveBondedStrength` below (SHED_PATH.md B2). Every shipped material
		 * carries 1.0 today, so no joint is derated yet — a below-1 factor is a
		 * per-material calibration a later slice sets when it has evidence.
		 */
		double BondFactor = 1.0;

		/**
		 * Whether this material carries load overwhelmingly in compression, so that
		 * its own tensile strength is a small fraction of its crushing strength.
		 *
		 * Masonry and concrete are compression members: they crush at many times the
		 * stress that pulls them apart, and a masonry profile whose tensile figure
		 * crept up near its compressive one is almost certainly mis-specified. Wood is
		 * the counter-example — genuinely tension-capable parallel to the grain, its
		 * tensile strength within a small factor of its compressive — and steel later
		 * will be another. The library sweep keys its "compressive >= 5x tensile"
		 * sanity check off this flag so it still bites for a bad MASONRY profile
		 * without condemning a legitimately tension-capable one. Defaults to true, the
		 * compression-member case that every structural masonry unit satisfies.
		 */
		bool bCompressionDominant = true;
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

	extern const FMaterialProfile Timber;

	/** Every material profile, so a sweep checks the whole library. */
	TArrayView<const FNamedMaterialProfile> AllMaterialProfiles();
}

namespace DestructionForce
{
	/*
	 * The connection x material weakest-link pairing (SHED_PATH.md B2).
	 *
	 * A joint between two pieces has a CONNECTION (its own directional strengths) and
	 * TWO material faces (each an FMaterialProfile, carrying its own strengths and a
	 * BondFactor). The joint fails at the WEAKEST link, per axis: the connection's own
	 * capacity, the BOND to each face (connection capacity derated by that face's
	 * BondFactor, which peels in tension and shear cohesion but not in compression
	 * bearing), and each material's OWN capacity. Effective per-axis capacity is
	 * min(ConnectionCap x min(BondFactor_A, BondFactor_B), MatCap_A, MatCap_B) on the
	 * tensile and shear-cohesion (bond) axes, and min(ConnectionCap, MatCap_A, MatCap_B)
	 * on the compression (bearing) axis, which BondFactor never touches. Friction and
	 * the shear ceiling are carried from the connection unchanged.
	 *
	 * STANDALONE FOR THIS SLICE — nothing in production calls it yet; the joint / LP
	 * strength path still reads the bare connection. Because every shipped material
	 * carries BondFactor 1.0, a single-material joint would read bit-identically through
	 * this function anyway, so wiring it in is a later slice's concern.
	 */
	FConnectionStrength EffectiveBondedStrength(
		const FConnectionStrength& Connection,
		const DestructionProfiles::FMaterialProfile& FaceA,
		const DestructionProfiles::FMaterialProfile& FaceB);
}
