// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ConnectionStrength.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"

/**
 * Build mode joint inference: the joint profile for a contact, as a pure function of the two
 * faces' materials and the interface normal (and optionally the boxes). Shared by interactive
 * build mode and the programmatic builders. World-free.
 */
namespace BuildMode
{
	/**
	 * The passive resting joint for two faces (BUILD_MODE_PLAN.md 2026-09-03). Fastening is never
	 * inferred; it is an explicit override.
	 *
	 * @param InterfaceNormalUnit Unit normal of the shared face; its dominant component decides
	 *                            bed (vertical) vs head/corner (horizontal).
	 */
	FConnectionStrength JointForContact(
		const DestructionProfiles::FMaterialProfile& FaceA,
		const DestructionProfiles::FMaterialProfile& FaceB,
		const FVector& InterfaceNormalUnit);

	/**
	 * Which masonry contact two pieces make (DESIGN §8). Bed and Corner are bonded; Head is the
	 * weak perpend and the fail-closed answer for degenerate input.
	 */
	enum class EMasonryContact : uint8
	{
		/** Vertical normal: the joint a course sits on. */
		Bed,

		/** Same-course joint between collinear pieces: the weak perpend. */
		Head,

		/** Bonded quoin: pieces meeting horizontally with long axes crossed. */
		Corner,
	};

	/**
	 * Classify bed, head or corner from the boxes and the normal (DESIGN §8). The normal alone
	 * cannot tell head from corner, so each piece's long axis (larger of ExtentCm.X/Y) is compared:
	 * differing axes mean a corner. Order-free. Fails closed to Head on degenerate input (see the
	 * .cpp for why that differs from the three-argument JointForContact).
	 */
	EMasonryContact ClassifyMasonryContact(
		const DestructionLayout::FPieceBox& A,
		const DestructionLayout::FPieceBox& B,
		const FVector& InterfaceNormalUnit);

	/**
	 * Orientation-aware resting joint: as the three-argument overload, except a masonry corner gets
	 * full GeneralPurposeMortar instead of the perpend (DESIGN §8). Use when boxes are available.
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
