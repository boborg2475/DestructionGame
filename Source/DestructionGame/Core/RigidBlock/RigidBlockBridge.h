// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/Structure.h"

/*
 * THE FSTRUCTURE BRIDGE, KEPT IN ITS OWN TRANSLATION UNIT ON PURPOSE. The rigid-block
 * solver (RigidBlockOracle.{h,cpp}) reads nothing but the plain problem structs and
 * Core/ConnectionStrength.h; that independence is the whole reason the sweep can catch
 * production being wrong (see the oracle header). This file is the ONE place the solver
 * meets production's geometry model — it includes Core/Structure.h and projects a live
 * FStructure into an FOracleProblem — so keeping it separate is what keeps the solver
 * core free of any structural dependency.
 */
namespace RigidBlockOracle
{
	/**
	 * THE BRIDGE THE FIXTURE SWEEP CALLS: project a live FStructure into an X-Z
	 * rigid-block problem.
	 *
	 * What it reads is exactly the data model DESIGN.md §7 says is the LP's input:
	 * pieces with mass and centre of mass, joints with centre, half extents, normal and
	 * strength profile. Removed pieces are skipped; joints that have GIVEN are skipped
	 * (a broken joint is out of the structure, so the oracle judges the graph as it
	 * stands now, latch included); a joint between two grounded pieces constrains
	 * nothing and is skipped.
	 *
	 * REFUSED, fail closed: a structure without complete geometry (a defaulted centre
	 * or rectangle would silently become a lever arm of "at the origin"), a live joint
	 * naming a removed piece (the known AddConnection tombstone hole), and — for a 2D
	 * structure only — any joint whose normal has a Y component: a 2D oracle projecting
	 * an out-of-plane joint would be a plausible number with wrong statics.
	 *
	 * THE 3D FLAG (FStructure::IsThreeDimensional) IS THE PERMISSION TO POSE 3D, NOT THE
	 * POSE. A flagged structure MAY carry its full Y geometry and be routed to the 3D
	 * oracle, so its Y-normal joints are carried rather than refused — but the bridge
	 * poses the CHEAPEST SOUND problem for what it actually contains: 2D whenever BOTH
	 * every joint it POSES (after the given / earth-to-earth skips) has an in-plane normal
	 * AND every Y that enters an equilibrium row (each ungrounded block's centroid, each
	 * posed patch centre) is one common Y; 3D otherwise. The two poses are NOT the same
	 * feasible set: the 3D friction rows are an inscribed octagon, 0.924x the exact
	 * Coulomb cone the 2D rows carry, so the 2D pose is the more accurate formulation and
	 * ~37x cheaper (DESIGN §8, 2026-09-16). An UNflagged structure is never promoted: a
	 * stray Y-normal joint is still refused.
	 *
	 * @return true and a filled problem, or false with the reason; OutProblem is
	 *         emptied on refusal so a caller who ignores the return solves nothing.
	 */
	bool BuildRigidBlockProblem(
		const FStructure& Structure,
		FOracleProblem& OutProblem,
		FString& OutWhyNot);

	/**
	 * THE SAME BRIDGE WITH A SET OF PIECES TREATED AS ABSENT — the "remainder without this
	 * body" projection the equilibrium gate needs to attribute a fall (Slice 2, D5 coarseness).
	 *
	 * A piece in ExcludedPieces contributes no block, and every joint that touches one is
	 * skipped rather than treated as the tombstone hole — an excluded body is deliberately
	 * gone, so a live joint to it is expected, not a fault. Every other refusal (incomplete
	 * geometry, a genuine tombstone on an INCLUDED piece, an out-of-plane normal) stands
	 * exactly as in the whole-structure form, which forwards to this with an empty set.
	 */
	bool BuildRigidBlockProblem(
		const FStructure& Structure,
		const TSet<int32>& ExcludedPieces,
		FOracleProblem& OutProblem,
		FString& OutWhyNot);

	/**
	 * THE GROUNDED-BOUNDARY BRIDGE — the regional collapse prover's pose (REGIONAL_PROVER_PLAN.md
	 * §2, review item 12). It projects only a NEIGHBOURHOOD of the structure into an oracle problem,
	 * with the neighbourhood's one-hop frontier pinned to the earth, so the LP can prove that a
	 * disturbed region collapses without paying for the whole structure.
	 *
	 * Interior RegionPieces are bridged exactly as the excluded-pieces form bridges an included
	 * piece — real mass, centroid, joints and EffectiveJointStrength weakest-link pairing. Frontier
	 * BoundaryPieces are bridged with Block.bGrounded forced true: they become earth, writing no
	 * equilibrium rows, so they can only ADD support versus reality. Only RegionPieces united with
	 * BoundaryPieces are included; every other piece is treated as absent exactly as ExcludedPieces
	 * are, and a joint touching an absent piece is skipped rather than faulted. A joint with two
	 * grounded ends (which now includes a real-to-boundary or boundary-to-boundary joint) is skipped
	 * as constraining nothing the earth does not already absorb. Every other refusal — incomplete
	 * geometry, a genuine tombstone on an INCLUDED piece, a 2D out-of-plane normal, a degenerate
	 * normal — stands exactly as in the excluded-pieces form.
	 *
	 * This is a SEPARATE function from BuildRigidBlockProblem on purpose: the shared bridge poses the
	 * whole-structure and excluded-pieces problems the flagship scenarios and the oracle sweep pin
	 * byte-for-byte, and forcing a boundary set grounded inside it would shift them. A piece present
	 * in BOTH sets is treated as boundary (grounded), the conservative reading.
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
