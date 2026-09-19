// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Layout.h"

/**
 * A STRUCTURE AS A FILE — the owner's ruling of 2026-09-18 that an authored building is DATA, not
 * C++ ("users will be able to build on a level in the game at some point and that won't need c++
 * code"). A level's building is a JSON file of boxes and materials under Content/Layouts/; the
 * joints are not in the file, they are found on load by SweepContacts, the same rule the
 * interactive build uses — so an authored level, a script-generated one and a saved player build
 * are the same thing read back through the same door.
 *
 * THE FORMAT, version 1:
 *
 *   {
 *     "format": "DestructionGame.Layout",
 *     "version": 1,
 *     "jointThicknessCm": 1.0,
 *     "threeDimensional": true,
 *     "pieces": [
 *       { "min": [0, 0, 0], "max": [21.5, 10.25, 6.5], "material": "ClayBrick", "grounded": true },
 *       ...
 *     ]
 *   }
 *
 * `min` / `max` are the box corners in cm on the world axes; `material` names a row of
 * DestructionProfiles::AllMaterialProfiles; `grounded` defaults to false. Mass is never in the
 * file — it is derived from the box and the material by PieceMassKg on load, so a piece cannot
 * weigh a different size than it sits. Serialize writes one piece per line so a file diffs.
 *
 * FAILS CLOSED: any refusal (unknown format, unknown material, a degenerate box, a piece with no
 * box) leaves the out layout EMPTY and, when asked, says why. WORLD-FREE: strings and boxes; the
 * file functions touch the disk and nothing else.
 */
namespace DestructionLayoutFile
{
	/** The `format` field every layout file carries. */
	extern const TCHAR* const FormatName;

	/** The version this build reads and writes. */
	constexpr int32 FormatVersion = 1;

	/** Content/Layouts/<Name>.json — where a level's layout file lives. */
	FString ContentPath(const TCHAR* Name);

	/**
	 * Read a layout from JSON text: the pieces, then the joints by SweepContacts, then the 3D flag.
	 *
	 * @param OutError When given, a one-line reason on refusal.
	 * @return true if a layout was read and its joints formed.
	 */
	bool Parse(const FString& Json, DestructionLayout::FBrickLayout& OutLayout, FString* OutError = nullptr);

	/** The pieces of a layout as JSON text in the format above. Joints are not written. */
	FString Serialize(const DestructionLayout::FBrickLayout& Layout, double JointThicknessCm);

	/** Parse the file at Path. */
	bool LoadFile(const FString& Path, DestructionLayout::FBrickLayout& OutLayout, FString* OutError = nullptr);

	/** Serialize to the file at Path, creating directories as needed. */
	bool SaveFile(const FString& Path, const DestructionLayout::FBrickLayout& Layout, double JointThicknessCm);
}
