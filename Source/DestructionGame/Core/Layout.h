// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Connection.h"
#include "Core/ConnectionStrength.h"
#include "Core/Structure.h"

/**
 * Builds an FStructure from a brick layout: which pairs touch, their interface area and normal.
 * MakeInterface is the only way to build a joint; RunningBond supplies the pairs it laid, so
 * there is no proximity tolerance. This header includes Structure.h, never the reverse, keeping
 * FStructure position-free.
 */
namespace DestructionLayout
{
	/** An axis-aligned box, cm. ExtentCm is the half-size, as FBox::GetExtent. */
	struct FPieceBox
	{
		FVector CentreCm = FVector::ZeroVector;
		FVector ExtentCm = FVector::ZeroVector;
	};

	/**
	 * Mass of the box in kg: g/cm3 x cm3 / 1000. No force conversion here (mass is unconverted,
	 * DESIGN.md §3). Degenerate input fails closed (Core.Layout.PieceMass).
	 */
	double PieceMassKg(const FPieceBox& Box, double DensityGramsPerCubicCm);

	/**
	 * Build the joint between two boxes, or refuse.
	 *
	 * The normal is the axis of separation, oriented toward B, never the centroid direction. A
	 * running-bond bed joint's centroid offset (11.25, 0, 7.5) would read as a head joint, putting
	 * gravity into shear against 0.2 MPa cohesion instead of compression: 41.5x the utilisation,
	 * silently. Emitting pair and normal together keeps them consistent.
	 *
	 * A joint needs separation by the joint thickness on exactly one axis and positive overlap on
	 * the other two (area = product of overlaps); two-axis separation is an edge, not a face.
	 * On refusal the out connection has zero area, so ignoring the return still reads as failed.
	 *
	 * @return true if the boxes form a face and a joint was written.
	 */
	bool MakeInterface(
		int32 HandleA,
		const FPieceBox& BoxA,
		int32 HandleB,
		const FPieceBox& BoxB,
		double JointThicknessCm,
		const FConnectionStrength& Strength,
		FConnection& OutConnection);

	/** How a running-bond wall finishes at its two ends. */
	enum class EWallEnd : uint8
	{
		/** Full bricks only, so alternate courses are one brick short and step in. */
		Ragged,

		/**
		 * Half bats at alternating course ends so both ends finish flush. Joint areas match the
		 * ragged wall's (105.0625 cm2 bed overlap); only the half bat's size and mass differ.
		 */
		Flush,
	};

	/** A running-bond wall, as data. */
	struct FRunningBondSpec
	{
		/** FULL brick dimensions, cm. UK metric standard is 21.5 x 10.25 x 6.5. */
		FVector BrickSizeCm = FVector(21.5, 10.25, 6.5);

		/** Mortar joint, cm. 1.0 gives the 22.5 x 11.25 x 7.5 coordinating grid. */
		double JointThicknessCm = 1.0;

		/** g/cm3, published values unconverted. */
		double DensityGramsPerCubicCm = 0.0;

		int32 CoursesHigh = 0;

		/** Full bricks in an even course. One brick wide has no bond. */
		int32 BricksPerCourse = 0;

		EWallEnd End = EWallEnd::Ragged;

		/** Joint profile for every connection. */
		FConnectionStrength Strength;
	};

	/** A wall: the solver's graph and the spawner's boxes, indexed by the same piece handles. */
	struct FBrickLayout
	{
		FStructure Structure;

		/** One box per piece handle, parallel to the structure's piece array. */
		TArray<FPieceBox> Boxes;
	};

	/** Lay a running-bond wall, bottom course grounded. Returns false and writes nothing for an invalid spec. */
	bool RunningBond(const FRunningBondSpec& Spec, FBrickLayout& OutLayout);
}
