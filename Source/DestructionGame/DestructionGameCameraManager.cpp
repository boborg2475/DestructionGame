// Copyright Epic Games, Inc. All Rights Reserved.


#include "DestructionGameCameraManager.h"

ADestructionGameCameraManager::ADestructionGameCameraManager()
{
	/*
	 * near-vertical pitch range: the flying pawn descends by looking down and
	 * moving forward, so it needs to be able to look almost straight down
	 */
	ViewPitchMin = -89.0f;
	ViewPitchMax = 89.0f;
}
