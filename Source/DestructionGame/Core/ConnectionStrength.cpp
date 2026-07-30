// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ConnectionStrength.h"

namespace DestructionForce
{
	namespace
	{
		/**
		 * How hard one axis is working: the stress it carries over the strength
		 * resisting it.
		 *
		 * Stress rather than raw force is the point. The same force through half
		 * the area is twice as punishing, so a force-only comparison would let
		 * thin joints survive loads that should part them.
		 */
		double AxisUtilisation(double ForceUnits, double StrengthMPa, double InterfaceAreaSqCm)
		{
			const double StressMPa = ForceUnits / (InterfaceAreaSqCm * ForceUnitsPerMPaSqCm);
			return StressMPa / StrengthMPa;
		}
	}

	double ComputeUtilisation(
		const FConnectionLoad& Load,
		const FConnectionStrength& Strength,
		double InterfaceAreaSqCm)
	{
		// A joint gives on whichever axis runs out first, so the worst governs.
		// Each axis is measured against its own strength — that separation is what
		// makes stone crush-resistant but brittle in shear, and it is the whole
		// reason this sits in front of Chaos's single strain threshold.
		return FMath::Max3(
			AxisUtilisation(Load.Compression, Strength.CompressiveStrengthMPa, InterfaceAreaSqCm),
			AxisUtilisation(Load.Shear, Strength.ShearStrengthMPa, InterfaceAreaSqCm),
			AxisUtilisation(Load.Tension, Strength.TensileStrengthMPa, InterfaceAreaSqCm));
	}
}
