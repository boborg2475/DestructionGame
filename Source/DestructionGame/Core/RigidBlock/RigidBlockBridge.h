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
	 * naming a removed piece (the known AddConnection tombstone hole), and any joint
	 * whose normal has a Y component — this is a 2D oracle and projecting an
	 * out-of-plane joint would be a plausible number with wrong statics.
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
}
