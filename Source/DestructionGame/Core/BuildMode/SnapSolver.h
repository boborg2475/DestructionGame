// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ConnectionStrength.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"

/**
 * BUILD MODE — the snap-candidate solver.
 *
 * The logical-building brain: given a piece being placed at a requested pose and
 * the nearby already-placed pieces (boxes + materials), return ranked candidate
 * poses. Each snap candidate carries the joints it would AUTO-FORM, whose profiles
 * are inferred (never hardcoded) via BuildMode::JointForContact from the two
 * faces' materials and the contact normal.
 *
 * WORLD-FREE, styled like Core/Layout: FPieceBox + doubles, no UWorld/UObject. One
 * direction of inclusion — this includes JointInference.h and Layout.h; nothing
 * under Tests/ is included by it.
 *
 * BEHAVIORS 2a/2b/2d/2e implement four snap kinds: brick RUNNING-BOND NEXT COURSE
 * (bed joint), brick SAME-COURSE END-TO-END (head joint), TIMBER CENTERED-ON a brick
 * and TIMBER EDGE-FLUSH to a brick face (both DryStone bearings). CR-2a adds the fifth,
 * brick CORNER RETURN — the quoin of DESIGN §8's 2026-09-15 ruling.
 *
 * THE BRICK GRID IS LONG-AXIS-AWARE, NOT X-AXIS-AWARE. Bed and head poses step along the
 * NEIGHBOUR'S long axis and are offered only when the placed brick is laid the SAME way,
 * so a wall running along Y grows exactly as one running along X does, reflected. A
 * placed brick laid ACROSS a brick-sized neighbour takes neither, and is offered the four
 * corner returns instead — one at each of the neighbour's ends, finishing flush with each
 * of its two width faces.
 *
 * ORIENTATION DECIDES THE POSE, CONTACT DECIDES THE JOINTS. Once a next-course pose is
 * offered, the brick BEDS ON EVERY brick-sized piece its underside has come to rest on —
 * a joint gap above that piece's top face, overlapping it in plan — whichever way that
 * piece runs. That is what bonds the course-1 stretcher a bricklayer laps OVER a corner
 * return to the return itself, and it is the same contact sweep a timber lintel uses to
 * find every support it spans.
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
