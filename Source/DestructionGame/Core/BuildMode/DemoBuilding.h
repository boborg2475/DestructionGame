// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/BuildMode/Placement.h"
#include "Core/BuildMode/SnapSolver.h"
#include "Core/Layout.h"

/**
 * BUILD MODE — a scripted demo building.
 *
 * The proof that the placement brain composes: a small, recognizable building
 * assembled entirely through BuildMode::PlacePiece and returned as a live
 * DestructionLayout::FBrickLayout that stands under SolveLoads. It lays a
 * running-bond brick wall (grounded bottom course, one staggered course
 * above) with a timber wall-plate bearing across the top, and returns the
 * per-step placement results so a caller can verify each snap.
 *
 * World-free, like the rest of Core/BuildMode: drives PlacePiece against
 * FPieceBox + doubles, never touches a UWorld. Every joint it forms is
 * X/Z-normal (no corner returns), so the structure is planar and needs no
 * SetThreeDimensional.
 */
namespace BuildMode
{
	/**
	 * Script the demo building into OutLayout, one PlacePiece at a time.
	 *
	 * @param OutLayout  The layout to grow. Grown from whatever it already holds.
	 * @param Settings   Brick geometry and snap radius handed to the solver each step.
	 * @return           One FPlacementResult per piece placed, in placement order.
	 */
	TArray<FPlacementResult> BuildDemoBuilding(
		DestructionLayout::FBrickLayout& OutLayout,
		const FSnapSettings& Settings = FSnapSettings{});
}
