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
		 * Whether this material carries load overwhelmingly in compression, so its own
		 * tensile strength is a small fraction of its crushing strength.
		 *
		 * Masonry and concrete are compression members: they crush at many times the
		 * stress that pulls them apart, so a masonry profile whose tensile figure
		 * creeps up near its compressive one is almost certainly mis-specified. Wood is
		 * the counter-example — tension-capable parallel to grain, within a small
		 * factor of its compressive strength — and steel will be another. The library
		 * sweep keys its "compressive >= 5x tensile" check off this flag so it still
		 * bites a bad masonry profile without condemning a legitimately tension-capable
		 * one. Defaults to true, the compression-member case every structural masonry
		 * unit satisfies.
		 */
		bool bCompressionDominant = true;
	};

	/**
	 * One row of the library. Adding a material is adding one of these.
	 *
	 * The profile is a reference to the shipped constant, never a copy — `&Row.Profile`
	 * is the address a piece carrying that material stores. A copy would make the
	 * obvious lookup (walk the library, compare the pointer a piece holds against the
	 * row's profile) silently answer "no such row" for every piece. A material is named
	 * by WHICH ROW it is, not by its current numbers (`BuildPieceMaterial` hands out
	 * references for exactly that reason), so the library must be askable by address.
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
	 * The connection x material weakest-link pairing (SHED_PATH.md B2).
	 *
	 * A joint has a connection (its own directional strengths) and two material faces
	 * (each an FMaterialProfile with its own strengths and a BondFactor). It fails at
	 * the weakest link per axis: effective capacity is min(ConnectionCap x
	 * min(BondFactor_A, BondFactor_B), MatCap_A, MatCap_B) on the tensile and
	 * shear-cohesion (bond) axes, and min(ConnectionCap, MatCap_A, MatCap_B) on
	 * compression (bearing), which BondFactor never touches. Friction and the shear
	 * ceiling carry from the connection unchanged.
	 *
	 * Standalone for this slice — nothing in production calls it yet; every shipped
	 * material carries BondFactor 1.0, so a single-material joint reads bit-identically
	 * through this function anyway.
	 */
	FConnectionStrength EffectiveBondedStrength(
		const FConnectionStrength& Connection,
		const DestructionProfiles::FMaterialProfile& FaceA,
		const DestructionProfiles::FMaterialProfile& FaceB);
}
