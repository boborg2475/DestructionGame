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
			/*
			 * Garbage in still has to fail closed. A NaN stress is worse than it
			 * looks: FMath::Max is (A >= B) ? A : B, and every comparison against
			 * NaN is false, so Max3 quietly discards it and returns whichever
			 * other axis happened to be lowest. The joint then reports a confident
			 * zero utilisation for an input nobody can interpret, which nothing
			 * downstream could detect. An infinite stress is genuinely failed.
			 */
			if (!FMath::IsFinite(Stress))
			{
				return TNumericLimits<double>::Max();
			}

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
		/*
		 * Fail closed on a joint that has no interface to carry anything across.
		 * Dividing by a zero area produces NaN, and NaN compares false against
		 * everything — including the > 1 test for failure — so the joint would
		 * report itself intact. Written as !(> 0) rather than <= 0 so that a NaN
		 * area is caught by the same branch instead of slipping past it.
		 */
		if (!(InterfaceAreaSqCm > 0.0))
		{
			return TNumericLimits<double>::Max();
		}

		const double CompressiveStress = StressMPa(Load.Compression, InterfaceAreaSqCm);
		const double TensileStress = StressMPa(Load.Tension, InterfaceAreaSqCm);
		const double ShearStress = StressMPa(Load.Shear, InterfaceAreaSqCm);

		/*
		 * Mohr-Coulomb: the bond, plus whatever friction the squeeze is worth.
		 * Only compression contributes — a joint being pulled open gains nothing,
		 * and FConnectionLoad guarantees Compression is zero whenever there is
		 * tension, so that falls out without a branch.
		 *
		 * Truncated at the material's own ceiling, because friction cannot help
		 * forever: past a point the material gives rather than the faces sliding.
		 * Left unbounded, capacity would climb with depth and joints at the base
		 * of a tall structure would become effectively uncuttable.
		 */
		const double ShearCapacityMPa = FMath::Min(
			Strength.ShearCohesionMPa + Strength.FrictionCoefficient * CompressiveStress,
			Strength.MaxShearStrengthMPa);

		/*
		 * A joint gives on whichever axis runs out first, so the worst governs.
		 * Each is measured against its own capacity — that separation is what makes
		 * stone crush-resistant but brittle in shear, and it is the whole reason
		 * this sits in front of Chaos's single strain threshold.
		 */
		return FMath::Max3(
			AxisUtilisation(CompressiveStress, Strength.CompressiveStrengthMPa),
			AxisUtilisation(ShearStress, ShearCapacityMPa),
			AxisUtilisation(TensileStress, Strength.TensileStrengthMPa));
	}
}
