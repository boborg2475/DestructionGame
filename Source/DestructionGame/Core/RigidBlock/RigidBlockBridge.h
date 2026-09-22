// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/Structure.h"

/*
 * The FStructure-to-oracle bridge, in its own translation unit so the solver core
 * (RigidBlockOracle.{h,cpp}) stays independent of production's geometry model. That
 * independence is what lets the sweep catch production being wrong.
 */
namespace RigidBlockOracle
{
	/**
	 * Project a live FStructure into a rigid-block problem (the LP input of DESIGN.md §7). Skips
	 * removed pieces, given joints, and joints between two grounded pieces.
	 *
	 * Refuses, fail closed: incomplete geometry (a default centre would become a lever arm at the
	 * origin), a live joint naming a removed piece (the AddConnection tombstone hole), and for a 2D
	 * structure any joint with a Y normal component (wrong statics).
	 *
	 * IsThreeDimensional permits a 3D pose but does not force it: the bridge poses 2D whenever
	 * every posed joint is in-plane and every centroid and patch centre shares one Y, else 3D. 2D
	 * is more accurate (3D friction is an inscribed octagon, 0.924x the cone) and ~37x cheaper
	 * (DESIGN §8). An unflagged structure is never promoted.
	 *
	 * @return true and a filled problem, or false with the reason; OutProblem is emptied on
	 *         refusal.
	 */
	bool BuildRigidBlockProblem(
		const FStructure& Structure,
		FOracleProblem& OutProblem,
		FString& OutWhyNot);

	/**
	 * The same bridge with ExcludedPieces treated as absent, used by the equilibrium gate to
	 * attribute a fall (D5). Joints to an excluded piece are skipped, not treated as tombstones;
	 * all other refusals stand. The whole-structure form forwards here with an empty set.
	 */
	bool BuildRigidBlockProblem(
		const FStructure& Structure,
		const TSet<int32>& ExcludedPieces,
		FOracleProblem& OutProblem,
		FString& OutWhyNot);

	/**
	 * Regional collapse prover's pose (REGIONAL_PROVER_PLAN.md §2): project only RegionPieces, with
	 * the one-hop frontier BoundaryPieces forced grounded, so the LP can prove a region collapses
	 * without solving the whole structure. Boundary pieces write no equilibrium rows, so they only
	 * add support versus reality. Every other piece is absent; joints to absent pieces and joints
	 * with two grounded ends are skipped. Other refusals match the excluded-pieces form.
	 *
	 * Separate from BuildRigidBlockProblem so the whole-structure poses stay byte-identical. A
	 * piece in both sets is treated as boundary (the conservative reading).
	 *
	 * @return true and a filled problem, or false with the reason; OutProblem is emptied on refusal.
	 */
	bool BuildRegionalProblem(
		const FStructure& Structure,
		const TSet<int32>& RegionPieces,
		const TSet<int32>& BoundaryPieces,
		FOracleProblem& OutProblem,
		FString& OutWhyNot);
}
