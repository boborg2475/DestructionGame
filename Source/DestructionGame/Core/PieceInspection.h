// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Structure.h"
#include "Core/StructureBinding.h"

/**
 * One joint on one piece: a row of the per-joint breakout. Every field is read back from the
 * solver, never re-derived, so the readout cannot drift from the break decision.
 */
struct FJointInspection
{
	int32 ConnectionIndex = INDEX_NONE;

	/** The neighbour at the far end. Still named after that piece is removed. */
	int32 OtherPieceIndex = INDEX_NONE;

	/** What this joint is to the inspected piece. */
	EJointRole Role = EJointRole::None;

	/**
	 * Force in uu, exactly as GetConnectionForce gives it. Not converted to newtons: that would be
	 * an open-coded second conversion (see ForceUnitsPerMPaSqCm). Formatting is the presenter's job.
	 */
	FVector ForceUu = FVector::ZeroVector;

	/**
	 * Bending moment about the joint centroid, uu.cm, exactly as GetConnectionMoment gives it.
	 * Needed to explain the utilisation. Zero means no eccentricity (see GetConnectionMoment).
	 */
	FVector MomentUuCm = FVector::ZeroVector;

	/** 0 unloaded, 1 at the limit, above 1 gives. */
	double Utilisation = 0.0;

	/**
	 * Whether the joint has given. Not inferable from force and utilisation: a given joint and an
	 * unloaded intact one both read 0.
	 */
	bool bHasGiven = false;

	/**
	 * Cascade pass that gave this joint, or INDEX_NONE. Only meaningful beside bHasGiven (see
	 * FStructure::GetBreakPass):
	 *
	 *     intact              bHasGiven false, INDEX_NONE
	 *     went with a piece   bHasGiven true,  INDEX_NONE
	 *     broke in pass N     bHasGiven true,  N >= 1
	 */
	int32 BreakPass = INDEX_NONE;
};

/**
 * Debugger view of one piece: its state and every joint on it. Lives in Core, with no world or
 * widget, so every number is testable; the menu widget must stay logic-free.
 */
struct FPieceInspection
{
	/** Whether the ref or handle named a live piece. Distinct from "no joints" (an isolated pad). */
	bool bIsPiece = false;

	int32 PieceIndex = INDEX_NONE;

	/**
	 * Whether the last solve answered for this piece (FStructure::HasSupportAnswer). Falling also
	 * means "not solved yet", so without this a never-solved wall would read as falling.
	 */
	bool bHasSupportAnswer = false;

	/** Falling when there is no answer. */
	EPieceSupport Support = EPieceSupport::Falling;

	/**
	 * Every joint on this piece, including given ones, in ascending connection order so the list is
	 * stable. Found by scan; one piece per click does not justify an adjacency index.
	 */
	TArray<FJointInspection> Joints;
};

/**
 * Per-joint breakout for one piece. Fails closed (bIsPiece false, no joints) on an invalid or
 * removed piece: FStructure keeps a removed piece's stale support answer, which a readout must not
 * show. Never emits a row for an unknown connection, so GetConnectionUtilisation's Max() sentinel
 * cannot appear.
 */
FPieceInspection InspectPiece(const FStructure& Structure, int32 PieceIndex);

/**
 * The same, entered by a click's ref. A ref naming another structure or a removed piece takes the
 * same fail-closed path.
 */
FPieceInspection InspectPiece(const FStructureBinding& Binding, const FPieceRef& Ref);
