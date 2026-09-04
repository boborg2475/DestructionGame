// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ConnectionStrength.h"
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
}
