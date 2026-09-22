// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ConnectionStrength.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"

/**
 * Build mode snap solver: given a piece at a requested pose and nearby placed pieces, return
 * ranked candidate poses, each with the joints it would form (profiles inferred via
 * JointForContact). World-free.
 *
 * Snap kinds: brick next course (bed), same course (head), corner return (quoin, DESIGN §8),
 * timber centred on a brick, and timber edge-flush to a brick face (both DryStone bearings).
 *
 * The grid follows the neighbour's long axis, so a wall along Y grows like one along X. A brick
 * laid across a neighbour gets the four corner returns instead. Orientation decides the pose;
 * contact decides the joints (a next-course brick beds on every brick-sized piece under it).
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

	/** One ranked snap option: pose, distance from the request, and joints formed. */
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
	 * @param NearbyBoxes     Already-placed pieces within reach.
	 * @param NearbyMaterials Their materials, parallel to NearbyBoxes.
	 */
	TArray<FSnapCandidate> SolveSnapCandidates(
		const DestructionLayout::FPieceBox& Placed,
		const DestructionProfiles::FMaterialProfile& PlacedMaterial,
		TArrayView<const DestructionLayout::FPieceBox> NearbyBoxes,
		TArrayView<const DestructionProfiles::FMaterialProfile> NearbyMaterials,
		const FSnapSettings& Settings);
}
