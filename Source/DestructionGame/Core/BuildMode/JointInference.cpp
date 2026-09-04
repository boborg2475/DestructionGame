// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/BuildMode/JointInference.h"
#include "Core/Profiles/ConnectionProfiles.h"

namespace BuildMode
{
	/*
	 * The passive resting joint two faces make just by touching, with no fastener.
	 *
	 * Masonry on masonry is mortar: a bed joint (interface normal ~ vertical, |Z|
	 * the dominant component) gets the strong GeneralPurposeMortar, while a head
	 * joint or corner return (normal ~ horizontal) gets the weak-perpend row, whose
	 * two bond axes are knocked down because a perpend is the weak link in real
	 * masonry.
	 *
	 * If EITHER face is not compression-dominant — timber today, and anything else
	 * that carries real tension later — the passive default is DryStone: a bearing
	 * that carries compression and friction but no tension. This is the fail-safe,
	 * least-committal choice. Timber pieces are held by explicit fasteners (a later
	 * slice's override); until one is placed, two timbers merely resting on each
	 * other should not be credited with a tensile bond they have not earned, so we
	 * fall back to the pure friction-plus-bearing joint rather than to mortar.
	 */
	FConnectionStrength JointForContact(
		const DestructionProfiles::FMaterialProfile& FaceA,
		const DestructionProfiles::FMaterialProfile& FaceB,
		const FVector& InterfaceNormalUnit)
	{
		if (!FaceA.bCompressionDominant || !FaceB.bCompressionDominant)
		{
			return DestructionProfiles::DryStone;
		}

		const double AbsZ = FMath::Abs(InterfaceNormalUnit.Z);
		const bool bBedJoint =
			AbsZ >= FMath::Abs(InterfaceNormalUnit.X) &&
			AbsZ >= FMath::Abs(InterfaceNormalUnit.Y);

		return bBedJoint
			? DestructionProfiles::GeneralPurposeMortar
			: DestructionProfiles::GeneralPurposeMortarPerpend;
	}
}
