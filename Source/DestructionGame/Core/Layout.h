// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Connection.h"
#include "Core/ConnectionStrength.h"
#include "Core/Structure.h"

/**
 * The connection graph PRODUCER: the bridge from a brick layout to an FStructure.
 *
 * Everything downstream of here CONSUMES interface areas and normals — ClassifyForce
 * needs a normal, ComputeUtilisation needs an area, and the two-tier support rule
 * reads the normal to decide whether a joint bears or hangs. Nothing until now
 * PRODUCED any of them: every fixture in the suite hand-wrote its own. A scenario
 * places bricks, and something has to decide which pairs touch, how big the shared
 * face is, and which way the interface faces.
 *
 * IT IS SPLIT IN TWO ON PURPOSE. MakeInterface owns areas, normals, orientation and
 * validation, and is the ONLY way to build a joint. RunningBond supplies the pairs,
 * and is the only part that would be replaced by a general contact-finding producer;
 * being generative it emits the pairs it knows it laid rather than searching for
 * them, so there is no proximity tolerance in the pair discovery at all.
 *
 * ONE DIRECTION OF INCLUSION. This header includes Structure.h; Structure.h must
 * never include this one. FStructure stays position-free — a piece is a mass and an
 * identity — which is exactly why the solver needs no world and the suite runs in
 * under a second. Geometry lives above the solver and hands down handles.
 */
namespace DestructionLayout
{
	/**
	 * An axis-aligned box, in centimetres. The whole geometric vocabulary of the
	 * producer.
	 *
	 * ExtentCm is HALF the size on each axis, matching FBox::GetExtent, so a box
	 * spans CentreCm - ExtentCm to CentreCm + ExtentCm.
	 */
	struct FPieceBox
	{
		FVector CentreCm = FVector::ZeroVector;
		FVector ExtentCm = FVector::ZeroVector;
	};

	/**
	 * What one piece of the given box weighs, in kilograms.
	 *
	 * THE ONE DERIVATION OF MASS FROM GEOMETRY. Density is g/cm3 and dimensions are cm,
	 * so the volume in cm3 divided by 1000 is kilograms; no force conversion belongs
	 * here, since DESIGN.md §3's 1 N = 100 uu is a property of forces and mass goes into
	 * Unreal unconverted.
	 *
	 * Degenerate boxes and densities fail closed. See Tests/LayoutTest.cpp,
	 * DestructionGame.Core.Layout.PieceMass, for what "closed" means for a mass.
	 */
	double PieceMassKg(const FPieceBox& Box, double DensityGramsPerCubicCm);

	/**
	 * Build the one joint between two boxes, or refuse.
	 *
	 * THE INTERFACE NORMAL IS THE AXIS OF SEPARATION, ORIENTED BY WHICH HANDLE IS B.
	 * It is never the direction between the two centroids, and that distinction is
	 * the whole reason this function exists rather than being open-coded at the call
	 * site. A running-bond bed joint has a centroid difference of (11.25, 0, 7.5) cm;
	 * normalised, its Z is 0.5547, which is BELOW cos 45 degrees — so a centroid
	 * normal makes every bed joint in the wall classify as a HEAD joint. The head
	 * tier is sign-blind, so each brick would then treat the joints above it as
	 * supports too, and gravity would resolve as shear against mortar's 0.2 MPa
	 * cohesion instead of compression against its 10 MPa. 41.5x the utilisation, an
	 * entirely wrong support graph, and nothing crashes: the wall stands there being
	 * wrong.
	 *
	 * THE 41.5 IS WORKED THROUGH, because an earlier draft of this comment said 79
	 * and that figure does not reproduce. On the spanning brick — 1333.5993125 uu
	 * over a 105.0625 cm2 bed joint — the axis-of-separation normal gives pure
	 * compression at 1.269339e-4 of capacity. The centroid normal (0.8320503, 0,
	 * 0.5547002) splits that same force into 739.7478 uu of compression and
	 * 1109.6217 uu of shear, so the shear stress is 1.056154e-3 MPa against a
	 * Mohr-Coulomb capacity of 0.2 + 0.6 x 7.041026e-4 = 0.20042246 MPa, and the
	 * worst axis comes out at 5.269638e-3. That is 41.5x. (74.8 is obtainable, but
	 * only as shear over compression BOTH read under the wrong normal, which is not
	 * the comparison this sentence makes.)
	 *
	 * The failure mode that matters is not a flipped normal but a normal
	 * INCONSISTENT WITH ITS A/B PAIRING. FStructure::GetJointRole turns the normal
	 * toward the piece it is asked about and the accumulation signs the force by which
	 * end is loaded, so a consistently flipped joint reports identical loads. Emitting
	 * the pair and the normal as one atomic value is what makes the inconsistent state
	 * inexpressible.
	 *
	 * Two boxes form a joint when they are separated on EXACTLY ONE axis, by the
	 * joint thickness, and overlap positively on the other two. The interface area is
	 * the product of those two overlaps. Anything else is not a face: boxes
	 * separated on two axes meet at an edge or a corner, which is the spurious
	 * diagonal pair that would change a joint's TIER and therefore where load ends
	 * up.
	 *
	 * FAILS CLOSED. On refusal the out connection is left with a zero interface area,
	 * so a caller that ignores the return value gets a joint that reads as failed
	 * rather than one that reads as fine.
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
		 * Half-length bricks at alternating course ends, so both faces finish flush.
		 *
		 * This is the one that makes a MIXED-SIZE structure: a producer that only ever
		 * emits identical pieces is not being tested very hard. The joint AREAS are the
		 * same as the ragged wall's — a half bat's bed overlap is still 105.0625 cm2
		 * and its end face is still 66.625 cm2 — so the difference is the second brick
		 * size and mass, not the geometry of any joint.
		 */
		Flush,
	};

	/** A running-bond wall, as data. */
	struct FRunningBondSpec
	{
		/** FULL brick dimensions, cm. UK metric standard is 21.5 x 10.25 x 6.5. */
		FVector BrickSizeCm = FVector(21.5, 10.25, 6.5);

		/**
		 * Mortar joint, cm. 1.0 with a standard brick gives the 22.5 x 11.25 x 7.5
		 * coordinating grid, which is what makes the half-brick offset land where it
		 * does.
		 */
		double JointThicknessCm = 1.0;

		/** g/cm3, Unreal's own unit for density — published values go in unconverted. */
		double DensityGramsPerCubicCm = 0.0;

		int32 CoursesHigh = 0;

		/** Full bricks in an even course. Courses one brick wide have no bond at all. */
		int32 BricksPerCourse = 0;

		EWallEnd End = EWallEnd::Ragged;

		/** The joint profile every connection in the wall is built with. */
		FConnectionStrength Strength;
	};

	/**
	 * A wall: the graph the solver wants, and the boxes whoever spawns actors wants,
	 * indexed by the same piece handles.
	 */
	struct FBrickLayout
	{
		FStructure Structure;

		/** One box per piece handle, parallel to the structure's piece array. */
		TArray<FPieceBox> Boxes;
	};

	/**
	 * Lay a running-bond wall, bottom course grounded.
	 *
	 * Rejects a spec that could not describe a wall, writing nothing.
	 *
	 * @return true if a wall was laid.
	 */
	bool RunningBond(const FRunningBondSpec& Spec, FBrickLayout& OutLayout);
}
