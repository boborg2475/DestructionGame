// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Connection.h"
#include "Core/ConnectionStrength.h"
#include "Core/Structure.h"

/**
 * The connection graph PRODUCER: the bridge from a brick layout to an FStructure.
 *
 * Everything downstream consumes interface areas and normals — ClassifyForce needs a
 * normal, ComputeUtilisation needs an area — and nothing produced them before this;
 * a scenario places bricks, and something has to decide which pairs touch, how big
 * the shared face is, and which way it faces.
 *
 * Split in two on purpose: MakeInterface owns areas, normals, orientation and
 * validation and is the only way to build a joint. RunningBond supplies the pairs and
 * is the only part a general contact-finding producer would replace — being
 * generative it emits the pairs it knows it laid, so pair discovery has no proximity
 * tolerance at all.
 *
 * One direction of inclusion: this header includes Structure.h, never the reverse.
 * FStructure stays position-free — a piece is a mass and an identity — which is why
 * the solver needs no world and the suite runs in under a second.
 */
namespace DestructionLayout
{
	/**
	 * An axis-aligned box, cm — the producer's whole geometric vocabulary. ExtentCm is
	 * HALF the size on each axis (matching FBox::GetExtent), so a box spans
	 * CentreCm - ExtentCm to CentreCm + ExtentCm.
	 */
	struct FPieceBox
	{
		FVector CentreCm = FVector::ZeroVector;
		FVector ExtentCm = FVector::ZeroVector;
	};

	/**
	 * What one piece of the given box weighs, in kilograms — the one derivation of
	 * mass from geometry. Density is g/cm3 and dimensions are cm, so volume in cm3 /
	 * 1000 is kilograms; no force conversion belongs here (DESIGN.md §3's 1 N = 100 uu
	 * is a property of forces, and mass goes into Unreal unconverted).
	 *
	 * Degenerate boxes and densities fail closed; see DestructionGame.Core.Layout.PieceMass.
	 */
	double PieceMassKg(const FPieceBox& Box, double DensityGramsPerCubicCm);

	/**
	 * Build the one joint between two boxes, or refuse.
	 *
	 * THE INTERFACE NORMAL IS THE AXIS OF SEPARATION, ORIENTED BY WHICH HANDLE IS B —
	 * never the direction between the two centroids. A running-bond bed joint's
	 * centroid difference is (11.25, 0, 7.5) cm, whose normalised Z of 0.5547 is below
	 * cos 45 degrees, so a centroid normal would classify every bed joint as a
	 * sign-blind HEAD joint: gravity then resolves as shear against mortar's 0.2 MPa
	 * cohesion instead of compression against its 10 MPa — 41.5x the utilisation, an
	 * entirely wrong support graph, and nothing crashes. (Worked on the spanning
	 * brick: the axis-of-separation normal gives 1.269339e-4 of capacity; the
	 * centroid normal splits the same force into a shear stress of 1.056154e-3 MPa
	 * against a Mohr-Coulomb capacity of 0.20042246 MPa, 5.269638e-3 — the 41.5x.)
	 *
	 * The failure that matters is a normal INCONSISTENT WITH ITS A/B PAIRING, not a
	 * flipped one: GetJointRole turns the normal toward whichever piece it is asked
	 * about, so a consistently flipped joint reports identical loads. Emitting the
	 * pair and the normal together is what makes the inconsistent state inexpressible.
	 *
	 * Two boxes form a joint when they are separated on exactly one axis, by the joint
	 * thickness, and overlap positively on the other two; the interface area is the
	 * product of those two overlaps. Separation on two axes is an edge or corner, not
	 * a face — a spurious diagonal that would change a joint's TIER.
	 *
	 * Fails closed: on refusal the out connection is left with a zero interface area,
	 * so a caller that ignores the return value gets a joint that reads as failed.
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
		 * The one that makes a mixed-size structure. Joint AREAS match the ragged
		 * wall's — a half bat's bed overlap is still 105.0625 cm2 — so the difference
		 * is the second brick's size and mass, not the geometry of any joint.
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
		 * coordinating grid, which is what makes the half-brick offset land correctly.
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
