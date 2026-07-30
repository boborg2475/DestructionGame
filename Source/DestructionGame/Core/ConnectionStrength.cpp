// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ConnectionStrength.h"

namespace DestructionForce
{
	namespace
	{
		/**
		 * Stress carried by a force, in MPa.
		 *
		 * Stress rather than raw force is the point. The same force through half
		 * the area is twice as punishing, so a force-only comparison would let
		 * thin joints survive loads that should part them.
		 */
		double StressMPa(double ForceUnits, double InterfaceAreaSqCm)
		{
			return ForceUnits / (InterfaceAreaSqCm * ForceUnitsPerMPaSqCm);
		}

		/**
		 * How hard one axis is working: its stress over the capacity resisting it.
		 *
		 * An axis with no capacity is handled explicitly rather than left to divide
		 * by zero. Unstressed it contributes nothing; carrying anything at all it
		 * has already gone. Dry stone makes this a live case rather than a
		 * theoretical one — it genuinely has zero tensile strength and zero
		 * cohesion, so 0/0 would otherwise produce a NaN, and since NaN compares
		 * false against everything the joint would read as intact rather than
		 * failed. Failing open is the wrong direction to be wrong in.
		 */
		double AxisUtilisation(double Stress, double CapacityMPa)
		{
			if (CapacityMPa > 0.0)
			{
				return Stress / CapacityMPa;
			}

			return Stress > 0.0 ? TNumericLimits<double>::Max() : 0.0;
		}
	}

	double ComputeUtilisation(
		const FConnectionLoad& Load,
		const FConnectionStrength& Strength,
		double InterfaceAreaSqCm)
	{
		const double CompressiveStress = StressMPa(Load.Compression, InterfaceAreaSqCm);
		const double TensileStress = StressMPa(Load.Tension, InterfaceAreaSqCm);
		const double ShearStress = StressMPa(Load.Shear, InterfaceAreaSqCm);

		// Mohr-Coulomb: the bond, plus whatever friction the squeeze is worth.
		// Only compression contributes — a joint being pulled open gains nothing,
		// and FConnectionLoad guarantees Compression is zero whenever there is
		// tension, so that falls out without a branch.
		const double ShearCapacityMPa =
			Strength.ShearCohesionMPa + Strength.FrictionCoefficient * CompressiveStress;

		// A joint gives on whichever axis runs out first, so the worst governs.
		// Each is measured against its own capacity — that separation is what makes
		// stone crush-resistant but brittle in shear, and it is the whole reason
		// this sits in front of Chaos's single strain threshold.
		return FMath::Max3(
			AxisUtilisation(CompressiveStress, Strength.CompressiveStrengthMPa),
			AxisUtilisation(ShearStress, ShearCapacityMPa),
			AxisUtilisation(TensileStress, Strength.TensileStrengthMPa));
	}
}
