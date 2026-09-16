// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ConnectionStrength.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"

/**
 * BUILD MODE — automatic joint inference.
 *
 * The judgment that BuildRealistic currently hand-codes per joint (every
 * `Join(A, B, DestructionProfiles::SomeProfile)` names its profile by hand)
 * expressed as a pure function of the two faces' MATERIALS and the INTERFACE
 * NORMAL between them. This is the "joint for this contact" helper the plan asks
 * the interactive build mode and the programmatic builders to share.
 *
 * WORLD-FREE, styled like Core/Layout: boxes and doubles, no UWorld/UObject. One
 * direction of inclusion — a test may include this, nothing under Tests/ is
 * included by it.
 */
namespace BuildMode
{
	/**
	 * The PASSIVE resting joint for two faces meeting across a given interface
	 * normal (owner-delegated ruling, BUILD_MODE_PLAN.md 2026-09-03). Fastening is
	 * an explicit override of a later slice and is never auto-inferred here.
	 *
	 * @param FaceA               One piece's material.
	 * @param FaceB               The other piece's material.
	 * @param InterfaceNormalUnit The unit normal of the shared face; its dominant
	 *                            component decides bed (vertical) vs head/corner
	 *                            (horizontal).
	 */
	FConnectionStrength JointForContact(
		const DestructionProfiles::FMaterialProfile& FaceA,
		const DestructionProfiles::FMaterialProfile& FaceB,
		const FVector& InterfaceNormalUnit);

	/**
	 * Which of the three masonry contacts two pieces make (DESIGN §8, 2026-09-15).
	 *
	 * Bed and Corner both carry a bonded masonry joint; Head is the weak perpend, and
	 * is also the answer every degenerate input falls to, so the enum's weakest
	 * masonry row is the one a fault lands on.
	 */
	enum class EMasonryContact : uint8
	{
		/** A horizontal bed: the normal is vertical, the joint the courses sit on. */
		Bed,

		/** A same-course head joint between collinear pieces — the weak perpend. */
		Head,

		/** A bonded quoin: two pieces meeting horizontally with their long axes crossed. */
		Corner,
	};

	/**
	 * Tell a bed joint from a same-course head joint from a bonded corner, using the
	 * two pieces' BOXES as well as the interface normal (owner-ratified ruling,
	 * DESIGN §8 2026-09-15).
	 *
	 * THE NORMAL ALONE CANNOT DO IT. A head joint's normal is parallel to both
	 * bricks' long axes; a quoin's is parallel to one and perpendicular to the other.
	 * The two are the same vector, so the footprints have to be read: a piece's long
	 * axis is whichever of ExtentCm.X / ExtentCm.Y is the larger, and the pair is a
	 * corner exactly when those two axes differ.
	 *
	 * Order-free: which piece is A never changes the answer, because the snap solver
	 * names the placed piece first while the shed sweep names the lower piece first.
	 *
	 * FAILS CLOSED TO Head on every degenerate input — see the .cpp for why that
	 * diverges deliberately from the three-argument JointForContact above.
	 */
	EMasonryContact ClassifyMasonryContact(
		const DestructionLayout::FPieceBox& A,
		const DestructionLayout::FPieceBox& B,
		const FVector& InterfaceNormalUnit);

	/**
	 * The PASSIVE resting joint, orientation-aware: as the three-argument overload,
	 * except that a bonded corner between two masonry faces earns full
	 * GeneralPurposeMortar instead of the perpend (DESIGN §8, 2026-09-15).
	 *
	 * This is the overload a caller with real geometry should use. The three-argument
	 * one remains for callers that hold only a normal; the two answer identically on
	 * every contact but the corner.
	 *
	 * @param BoxA The box of the piece whose material is FaceA.
	 * @param BoxB The box of the piece whose material is FaceB.
	 */
	FConnectionStrength JointForContact(
		const DestructionProfiles::FMaterialProfile& FaceA,
		const DestructionProfiles::FMaterialProfile& FaceB,
		const FVector& InterfaceNormalUnit,
		const DestructionLayout::FPieceBox& BoxA,
		const DestructionLayout::FPieceBox& BoxB);
}
