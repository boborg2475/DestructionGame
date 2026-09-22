// Copyright Epic Games, Inc. All Rights Reserved.

#include "RequiredContent.h"

/*
 * The required-content table: one row per path this module resolves from C++. File-local names
 * carry a RequiredContent prefix because a unity build merges files, so two anonymous same-named
 * arrays would collide (see CURRENT_STATE.md).
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
		 * The session's keyboard: camera modifier, the eight toolbar shortcuts, and their mapping
		 * context. The modifier needs its own row — nothing else resolves it, so a missing one
		 * stops the camera turning entirely.
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

		// The load overlay's three bands. See RequiredContent.h for why there are exactly three.
		DestructionContent::BrickLoadComfortableMaterialPath,
		DestructionContent::BrickLoadCautionMaterialPath,
		DestructionContent::BrickLoadCriticalMaterialPath,

		/*
		 * One row per colour slot, spelled out rather than looped: this table is a readable list of
		 * paths. The paths are named once, in RequiredContent.h.
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
