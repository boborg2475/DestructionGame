// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/BuildMode/SnapSolver.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"

/**
 * BUILD MODE — the placement API.
 *
 * The layer above the snap-candidate solver: it turns a snap into a LIVE piece in a
 * growing DestructionLayout::FBrickLayout. Given a requested pose it runs the solver
 * against the pieces already there, adopts the piece at the best-ranked snapped pose
 * (mass derived from geometry + density), and forms that candidate's joints as real
 * FConnections via DestructionLayout::MakeInterface — so the structure grows by one
 * live, jointed piece and is immediately subject to the integrity/collapse system.
 *
 * WORLD-FREE, styled like Core/Layout and SnapSolver: FPieceBox + doubles, no
 * UWorld/UObject. Includes SnapSolver.h, Layout.h and Profiles; nothing under Tests/.
 */
namespace BuildMode
{
	/** The outcome of placing one piece: its new handle, the snap chosen, joints formed. */
	struct FPlacementResult
	{
		int32 PieceHandle = INDEX_NONE;
		ESnapKind Kind = ESnapKind::Free;
		int32 JointsFormed = 0;
	};

	/**
	 * Place one piece into a growing layout at its best-ranked snapped pose.
	 *
	 * DELIBERATELY DOES NOT CALL SetThreeDimensional. Whether a build is planar or 3D is a
	 * structure-level intent the caller/harness owns, not a per-placement decision — so a
	 * build that forms out-of-plane (Y-normal) joints must SetThreeDimensional(true) on the
	 * structure BEFORE the break gate runs, or `SolveAndBreak`'s below-cap LP gate refuses
	 * the whole problem on the first Y normal and falls back to the router. `SolveLoads` is
	 * unaffected either way — the router reads normals directly and is dimension-agnostic.
	 * In a corner the Y normals are the Y leg's HEAD joints; the quoin itself is X-normal,
	 * so a single-leg wall needs no such call. The world path states it at the door instead
	 * (UDestructionStructureSubsystem::BeginBuild); this layout path leaves it to its caller.
	 *
	 * @param InOutLayout        The structure + parallel Boxes to grow. Handles index both.
	 * @param RequestedCentreCm  Where the caller asked to put the piece.
	 * @param ExtentCm           Half-size of the piece being placed.
	 * @param Material           What the placed piece is made of.
	 * @param bGrounded          Whether the placed piece rests on the earth.
	 * @param Settings           Brick geometry and snap radius for the solver.
	 */
	FPlacementResult PlacePiece(
		DestructionLayout::FBrickLayout& InOutLayout,
		const FVector& RequestedCentreCm,
		const FVector& ExtentCm,
		const DestructionProfiles::FMaterialProfile& Material,
		bool bGrounded,
		const FSnapSettings& Settings);
}
