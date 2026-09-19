// Copyright Epic Games, Inc. All Rights Reserved.

#include "RequiredContent.h"

/*
 * The required-content table — one row per path this module resolves from C++.
 *
 * File-local names carry a RequiredContent prefix. An anonymous namespace is private to a
 * translation unit rather than to a file, and a unity build merges many files into one, at
 * which point two file-local arrays of the same name are one declaration twice. See
 * CURRENT_STATE.md, where the collisions this has already caused are recorded.
 */
namespace
{
	const TCHAR* const RequiredContentRows[] = {
		DestructionContent::MoveActionPath,
		DestructionContent::LookActionPath,
		DestructionContent::MouseLookActionPath,
		DestructionContent::AscendActionPath,
		DestructionContent::InspectPieceActionPath,
		DestructionContent::HoverPieceActionPath,
		DestructionContent::DefaultMappingContextPath,
		DestructionContent::MouseLookMappingContextPath,

		/*
		 * The session's keyboard: the modifier the camera is chorded to, the eight shortcuts the
		 * toolbar draws, and the context that maps them. The modifier earns its row on its own —
		 * nothing resolves it onto a CDO, so this table is the only place that would notice it
		 * had gone, and what breaks is the camera failing to turn at all, not one key misfiring.
		 */
		DestructionContent::LookModifierActionPath,
		DestructionContent::SessionToggleModeActionPath,
		DestructionContent::SessionPieceBrickActionPath,
		DestructionContent::SessionPiecePlateActionPath,
		DestructionContent::SessionPieceLintelActionPath,
		DestructionContent::SessionSnapToggleActionPath,
		DestructionContent::SessionCourseUpActionPath,
		DestructionContent::SessionCourseDownActionPath,
		DestructionContent::SessionRunActionPath,
		DestructionContent::SessionMappingContextPath,

		DestructionContent::BrickPlaceholderMeshPath,
		DestructionContent::BrickHoverMaterialPath,
		DestructionContent::BrickSelectedMaterialPath,
		DestructionContent::BrickInspectedMaterialPath,

		/* The load overlay's three bands. See RequiredContent.h for why there are exactly three. */
		DestructionContent::BrickLoadComfortableMaterialPath,
		DestructionContent::BrickLoadCautionMaterialPath,
		DestructionContent::BrickLoadCriticalMaterialPath,

		/*
		 * One row per colour slot, spelled out rather than spliced in from the array: this table
		 * is a list of paths, and a loop appending six of them would make it unreadable at a
		 * glance, the one thing it's for. The paths are still named once, in RequiredContent.h.
		 */
		DestructionContent::BrickNeighbourMaterialPaths[0],
		DestructionContent::BrickNeighbourMaterialPaths[1],
		DestructionContent::BrickNeighbourMaterialPaths[2],
		DestructionContent::BrickNeighbourMaterialPaths[3],
		DestructionContent::BrickNeighbourMaterialPaths[4],
		DestructionContent::BrickNeighbourMaterialPaths[5],

		DestructionContent::ShedBrickMaterialPath,
		DestructionContent::ShedTimberMaterialPath
	};
}

TArrayView<const TCHAR* const> DestructionContent::RequiredContentPaths()
{
	return TArrayView<const TCHAR* const>(RequiredContentRows);
}
