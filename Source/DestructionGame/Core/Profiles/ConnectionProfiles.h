// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/ConnectionStrength.h"

/**
 * The shared connection profile library.
 *
 * ONE PLACE where a joint type's real-world strengths live, so that retuning
 * mortar changes mortar everywhere rather than in whichever file the author
 * happened to have open. The values and their citations are in
 * ConnectionProfiles.cpp.
 */
namespace DestructionProfiles
{
	/**
	 * What kind of joint a profile is, which decides WHICH physical invariants
	 * apply to it.
	 *
	 * Data, not code: the class is a field on the row rather than a branch
	 * anywhere. It is also what lets a test fixture be machine-recognisable, so
	 * an unbreakable joint can never quietly reach a scenario.
	 */
	enum class EConnectionProfileClass : uint8
	{
		/** A bond that grips as well as rubs — mortar. Real cohesion, real friction. */
		Bonded,

		/** No bond at all — dry stone. Carries shear purely by friction. */
		Frictional,

		/** Nail, screw, bolt. A discrete fastener does not care how hard the faces
		    are pressed together, so mu is exactly zero. */
		MechanicalFastener,

		/** TEST FIXTURE ONLY. Must never be used by a scenario. */
		TestFixture,
	};

	/** One row of the library. Adding a profile is adding one of these. */
	struct FNamedConnectionProfile
	{
		const TCHAR* Name = nullptr;
		EConnectionProfileClass Class = EConnectionProfileClass::TestFixture;
		FConnectionStrength Strength;
	};

	extern const FConnectionStrength GeneralPurposeMortar;
	extern const FConnectionStrength LimeMortar;
	extern const FConnectionStrength DryStone;
	extern const FConnectionStrength Nail;
	extern const FConnectionStrength Screw;
	extern const FConnectionStrength Bolt;

	/** TEST FIXTURE ONLY — a joint that never gives. Never ship it in a scenario. */
	extern const FConnectionStrength Unbreakable;

	/**
	 * Every connection profile, so a sweep can check the whole library rather
	 * than whichever entries a test remembered to name.
	 */
	TArrayView<const FNamedConnectionProfile> AllConnectionProfiles();
}
