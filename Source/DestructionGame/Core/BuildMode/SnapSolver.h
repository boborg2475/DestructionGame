// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ConnectionStrength.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"

/**
 * BUILD MODE — the snap-candidate solver.
 *
 * Given a piece being placed at a requested pose and the nearby already-placed
 * pieces (boxes + materials), return ranked candidate poses. Each candidate
 * carries the joints it would auto-form, profiles inferred (never hardcoded)
 * via JointForContact from the two faces' materials and the contact normal.
 *
 * World-free, styled like Core/Layout: FPieceBox + doubles, no UWorld/UObject.
 * Includes JointInference.h and Layout.h; nothing under Tests/ includes it.
 *
 * Five snap kinds: brick running-bond next course (bed joint), brick
 * same-course end-to-end (head joint), brick corner return (the quoin of
 * DESIGN §8's 2026-09-15 ruling), timber centered-on a brick, and timber
 * edge-flush to a brick face (both DryStone bearings).
 *
 * The brick grid is long-axis-aware, not X-axis-aware: bed and head poses step
 * along the neighbour's long axis, offered only when the placed brick is laid
 * the same way, so a wall along Y grows like one along X, reflected. A brick
 * laid across a brick-sized neighbour takes neither and is offered the four
 * corner returns instead, one at each neighbour end, flush with each width face.
 *
 * Orientation decides the pose, contact decides the joints: once a next-course
 * pose is offered, the brick beds on every brick-sized piece its underside has
 * come to rest on, whichever way that piece runs — the same contact sweep that
 * bonds a lapped stretcher to a corner return, or finds every support a lintel
 * spans.
 */
namespace BuildMode
{
	enum class ESnapKind : uint8
	{
		Free,
		BrickNextCourse,
		BrickSameCourse,
		BrickCornerReturn,
		TimberCentered,
		TimberEdgeFlush
	};

	/** Brick coordinating geometry the solver snaps to. Defaults are the UK metric standard. */
	struct FSnapSettings
	{
		FVector BrickSizeCm = FVector(21.5, 10.25, 6.5);
		double JointThicknessCm = 1.0;
		double SnapRadiusCm = 30.0;
	};

	/** One joint a candidate would form, to a nearby piece (index into NearbyBoxes). */
	struct FFormedJoint
	{
		int32 OtherPieceIndex = INDEX_NONE;
		FConnectionStrength Profile;
	};

	/** A single ranked snap option: a pose, its distance from the requested pose, and the joints it forms. */
	struct FSnapCandidate
	{
		ESnapKind Kind = ESnapKind::Free;
		FVector CentreCm = FVector::ZeroVector;
		double OffsetFromRequestedCm = 0.0;
		TArray<FFormedJoint> Joints;
	};

	/**
	 * Rank candidate poses for a piece being placed.
	 *
	 * @param Placed          The piece being placed; ExtentCm is HALF size, CentreCm is the requested pose.
	 * @param PlacedMaterial  The placed piece's material.
	 * @param NearbyBoxes     Already-placed pieces within reach.
	 * @param NearbyMaterials Their materials, parallel to NearbyBoxes.
	 * @param Settings        Brick geometry and snap radius.
	 */
	TArray<FSnapCandidate> SolveSnapCandidates(
		const DestructionLayout::FPieceBox& Placed,
		const DestructionProfiles::FMaterialProfile& PlacedMaterial,
		TArrayView<const DestructionLayout::FPieceBox> NearbyBoxes,
		TArrayView<const DestructionProfiles::FMaterialProfile> NearbyMaterials,
		const FSnapSettings& Settings);
}
