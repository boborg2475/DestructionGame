// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ConnectionLoad.h"

namespace DestructionForce
{
	FConnectionLoad ClassifyForce(const FVector& Force, const FVector& InterfaceNormal)
	{
		FConnectionLoad Load;

		FVector UnitNormal = InterfaceNormal;
		if (!UnitNormal.Normalize())
		{
			// Degenerate normal: no interface plane to resolve against, so report no load rather than dividing by zero.
			return Load;
		}

		/*
		 * Split the force into the part along the interface normal and the part in
		 * the interface plane; everything below follows from that split, which is
		 * why the joint's orientation decides the load type, not world axes.
		 */
		const double NormalComponent = FVector::DotProduct(Force, UnitNormal);
		const FVector ShearForce = Force - (NormalComponent * UnitNormal);

		if (NormalComponent > 0.0)
		{
			// Along +Normal: pulling the two faces apart.
			Load.Tension = NormalComponent;
		}
		else if (NormalComponent < 0.0)
		{
			// Against the normal: pressing the two faces together.
			Load.Compression = -NormalComponent;
		}

		Load.Shear = ShearForce.Size();

		return Load;
	}
}
