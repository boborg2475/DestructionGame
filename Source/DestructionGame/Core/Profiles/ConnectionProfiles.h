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

	/**
	 * One row of the library. Adding a profile is adding one of these.
	 *
	 * THE STRENGTH IS A REFERENCE TO THE SHIPPED CONSTANT, NEVER A COPY OF IT, so `&Row.Strength`
	 * IS the address a joint fastened with that row carries. It was a copy, and a copy makes the
	 * obvious lookup — walk the library, compare the pointer against the row's profile — answer "no
	 * such row" for every joint in the game, quietly and without a cast to warn anybody. That is
	 * exactly the defect FNamedMaterialProfile::Profile was fixed for; this is the same fix on the
	 * connection library.
	 */
	struct FNamedConnectionProfile
	{
		const TCHAR* Name = nullptr;
		EConnectionProfileClass Class = EConnectionProfileClass::TestFixture;
		const FConnectionStrength& Strength;
	};

	extern const FConnectionStrength GeneralPurposeMortar;

	/**
	 * The weak-perpend row — general purpose mortar in a vertical head/corner joint, its two bond axes
	 * knocked down (owner-approved item 6b). See ConnectionProfiles.cpp for why a perpend is the weak link.
	 */
	extern const FConnectionStrength GeneralPurposeMortarPerpend;

	extern const FConnectionStrength LimeMortar;
	extern const FConnectionStrength DryStone;
	extern const FConnectionStrength Nail;
	extern const FConnectionStrength Screw;
	extern const FConnectionStrength Bolt;

	/** TEST FIXTURE ONLY — a joint that never gives. Never ship it in a scenario. */
	extern const FConnectionStrength Unbreakable;

	/**
	 * TEST FIXTURE ONLY — no cohesion at all, but a real tensile bond. Never ship it.
	 *
	 * Not a material. It exists because `DryStone` cannot answer the question the
	 * composite-depth work needs answered; see ConnectionProfiles.cpp.
	 */
	extern const FConnectionStrength CohesionlessBond;

	/**
	 * Every connection profile, so a sweep can check the whole library rather
	 * than whichever entries a test remembered to name.
	 */
	TArrayView<const FNamedConnectionProfile> AllConnectionProfiles();

	/**
	 * Which library row a joint's STRENGTH is, matched FIELD FOR FIELD — or null for one this
	 * library never shipped.
	 *
	 * BY VALUE BECAUSE THE IDENTITY IS ALREADY GONE BY THE TIME ANYONE CAN ASK. An FConnection
	 * stores a COPY of the profile it was made with (FStructure::AddConnection takes the strength,
	 * not a pointer to it), so a joint in a built wall has no address to compare — and the question
	 * a readout has to answer is precisely "which shipped row fastens this". Matching the five
	 * fields back to the row is the honest route to the name, and it is why the rows above had to
	 * become references first: the answer is the EXTERN, so `&FindConnectionProfileRow(S)->Strength`
	 * is the library's own address rather than a pointer into a private copy.
	 *
	 * TWO ROWS WITH IDENTICAL FIELDS WOULD BE AMBIGUOUS, and this returns the FIRST. No two shipped
	 * rows are identical today — the closest pair, DryStone and the CohesionlessBond fixture, differ
	 * on tension — but a retune that collapsed two rows onto one set of numbers would silently make
	 * every joint of the second read as the first. There is nothing to be done about that here: two
	 * profiles with equal fields are the same physics, and it is the LIBRARY that would have gone
	 * wrong.
	 *
	 * AN EXACT COMPARISON, NOT A TOLERANCE. The strength being looked up is a copy of a library row
	 * rather than the result of arithmetic on one, so the bits are the row's bits; a near-match
	 * would be answering a question nobody asked — "which row is this LIKE" — and a NaN field, which
	 * compares equal to nothing, correctly finds no row at all.
	 */
	const FNamedConnectionProfile* FindConnectionProfileRow(const FConnectionStrength& Strength);
}
