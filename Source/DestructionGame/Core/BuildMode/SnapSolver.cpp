// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/BuildMode/SnapSolver.h"
#include "Core/BuildMode/JointInference.h"

namespace BuildMode
{
	namespace
	{
		/*
		 * A box is brick-sized when its full dimensions (twice the half-extent) match
		 * the coordinating brick within a loose centimetre tolerance, in either
		 * in-plane orientation — the same brick turned 90 degrees to run along Y is
		 * not a different piece, it's what a corner return and a Y-running wall are
		 * built from. Rejecting the swapped footprint left a rotated brick with
		 * nothing but the Free fallback. Non-brick pieces (timber) simply fail the
		 * gate; their own snap kinds handle them below.
		 */
		bool IsBrickSized(const FVector& ExtentCm, const FVector& BrickSizeCm)
		{
			const FVector FullCm = ExtentCm * 2.0;
			const FVector RotatedBrickCm(BrickSizeCm.Y, BrickSizeCm.X, BrickSizeCm.Z);
			return FullCm.Equals(BrickSizeCm, 0.5) || FullCm.Equals(RotatedBrickCm, 0.5);
		}

		/*
		 * Which in-plane axis a brick-sized box runs along, as a unit vector. The
		 * running bond steps along the neighbour's length however the wall is turned,
		 * so the grid is expressed in this axis rather than in X — an X-only grid
		 * can't grow a wall along Y, half of what a corner is for.
		 *
		 * Only ever asked of a box that already passed IsBrickSized, so the two
		 * in-plane dimensions differ by a whole brick's width and the comparison is
		 * never close. Written `!(Y > X)` rather than `X >= Y` so a NaN extent lands
		 * on X rather than whichever branch the comparison order favoured.
		 */
		FVector LongAxisUnit(const FVector& ExtentCm)
		{
			return !(ExtentCm.Y > ExtentCm.X) ? FVector(1.0, 0.0, 0.0) : FVector(0.0, 1.0, 0.0);
		}

		/*
		 * Whether a placed box at Pose (half-extent PlacedExtentCm) shares volume with
		 * any nearby box: a strict overlap on all three axes at once,
		 * Abs(dCentre) < sum-of-half-extents on X, Y and Z. The strict < is what
		 * distinguishes this from a legitimate joint contact, which is gapped (or
		 * exactly touching) on one axis — a next-course pose clears in Z (7.5 > 6.5),
		 * a same-course pose clears in X (22.5 > 21.5).
		 */
		bool InterpenetratesAny(
			const FVector& Pose,
			const FVector& PlacedExtentCm,
			TArrayView<const DestructionLayout::FPieceBox> NearbyBoxes)
		{
			for (const DestructionLayout::FPieceBox& Other : NearbyBoxes)
			{
				const FVector Sum = PlacedExtentCm + Other.ExtentCm;
				if (FMath::Abs(Pose.X - Other.CentreCm.X) < Sum.X
					&& FMath::Abs(Pose.Y - Other.CentreCm.Y) < Sum.Y
					&& FMath::Abs(Pose.Z - Other.CentreCm.Z) < Sum.Z)
				{
					return true;
				}
			}
			return false;
		}
	}

	/*
	 * Brick-on-brick running-bond next course, plus the Free fallback.
	 *
	 * For each brick-sized nearby piece the placed brick could rest on, offer the
	 * next-course pose: one course up (a brick beds on the one below) and half a
	 * brick across (running bond staggers alternate courses so head joints never
	 * line up). The stagger is taken on whichever side the requested cursor leans
	 * toward, so the offered pose is nearest where the player is pointing.
	 *
	 * The bed joint's profile is inferred, never named here: JointForContact is
	 * fed a vertical (+Z) normal so it returns the strong bed mortar, not a perpend.
	 *
	 * A brick in running bond straddles the two below it, so per-neighbour poses
	 * that coincide are coalesced into one candidate carrying both bed joints.
	 * Snaps are ordered nearest-first, Free appended last, so any in-range snap
	 * outranks placing exactly where requested.
	 */
	TArray<FSnapCandidate> SolveSnapCandidates(
		const DestructionLayout::FPieceBox& Placed,
		const DestructionProfiles::FMaterialProfile& PlacedMaterial,
		TArrayView<const DestructionLayout::FPieceBox> NearbyBoxes,
		TArrayView<const DestructionProfiles::FMaterialProfile> NearbyMaterials,
		const FSnapSettings& Settings)
	{
		TArray<FSnapCandidate> Candidates;

		/*
		 * What the placed piece is decides which snap kinds it can take. A
		 * brick-sized piece bonds into the running-bond grid (bed + head); a timber
		 * piece — not compression-dominant — bears instead, centred on the support
		 * rather than staggering into a course.
		 */
		const bool bPlacedIsBrick = IsBrickSized(Placed.ExtentCm, Settings.BrickSizeCm);
		const bool bPlacedIsTimber = !PlacedMaterial.bCompressionDominant;

		/*
		 * Two sources that agree inside the brick-sized gate: the half-stagger across
		 * is half a coordinating brick pitch (Settings: brick length + head joint);
		 * the course rise up comes from the actual box half-heights plus the bed
		 * joint. For real bricks these match Layout's 22.5 x 11.25 x 7.5 grid — the
		 * gate is what guarantees the two boxes are that brick.
		 *
		 * Both are lengths of the brick, not of an axis: BrickSizeCm.X names the
		 * brick's length, and the pitch it gives is stepped along whichever world
		 * axis the neighbour happens to run along.
		 */
		const double HalfStaggerCm = (Settings.BrickSizeCm.X + Settings.JointThicknessCm) / 2.0;

		// Same-course pitch: one brick length plus one head joint, at the same height.
		const double SameCoursePitchCm = Settings.BrickSizeCm.X + Settings.JointThicknessCm;

		/*
		 * Emit a snap at Centre, or merge into a coincident one — a brick in running
		 * bond straddles the two below it, so per-neighbour poses that coincide are
		 * coalesced into one candidate carrying every bed joint; the same union serves
		 * any other kind whose poses meet. Poses beyond the snap radius are dropped.
		 */
		auto EmitOrMerge =
			[&Candidates, &Placed, &NearbyBoxes, &Settings](
				ESnapKind Kind, const FVector& Centre, const TArray<FFormedJoint>& Joints)
		{
			const double Offset = (Centre - Placed.CentreCm).Size();
			if (Offset > Settings.SnapRadiusCm)
			{
				return;
			}

			/*
			 * Drop a pose whose box would occupy a cell another piece already fills. The
			 * placed half-extent is measured (not the snap geometry); a joint contact is
			 * gapped on one axis and survives this.
			 */
			if (InterpenetratesAny(Centre, Placed.ExtentCm, NearbyBoxes))
			{
				return;
			}

			FSnapCandidate* Existing = Candidates.FindByPredicate(
				[&Centre](const FSnapCandidate& C)
				{
					return C.CentreCm.Equals(Centre, KINDA_SMALL_NUMBER);
				});
			if (Existing != nullptr)
			{
				/*
				 * Union by OtherPieceIndex: a placed piece forms at most one joint to any one
				 * neighbour at a single pose, so a second joint to an index already present
				 * is a true duplicate (as when a plank's centred and edge-flush poses
				 * coincide). Distinct indices — the brick straddle, the bed+head pair, a
				 * plate's separate bearings — carry different neighbours and are all kept.
				 */
				for (const FFormedJoint& Joint : Joints)
				{
					const bool bAlreadyJoined = Existing->Joints.ContainsByPredicate(
						[&Joint](const FFormedJoint& Have)
						{
							return Have.OtherPieceIndex == Joint.OtherPieceIndex;
						});
					if (!bAlreadyJoined)
					{
						Existing->Joints.Add(Joint);
					}
				}
				return;
			}

			FSnapCandidate Candidate;
			Candidate.Kind = Kind;
			Candidate.CentreCm = Centre;
			Candidate.OffsetFromRequestedCm = Offset;
			Candidate.Joints = Joints;
			Candidates.Add(MoveTemp(Candidate));
		};

		/*
		 * Every joint the placed piece forms by resting at Pose, found by contact
		 * rather than by which neighbour fixed the pose: a brick-sized piece j
		 * qualifies when the placed box overlaps it in both X and Y and its
		 * underside sits exactly one joint above j's top face.
		 *
		 * Contact, not the pose's own neighbour, because a plank spans every brick
		 * beneath it, not just the one it was centred on, and a brick in running
		 * bond straddles two below it — at a corner one of those is the return laid
		 * across the leg, the alternate-course lap that lets a quoin carry load from
		 * above (DESIGN §8's 2026-09-15 ruling, review finding B2). Which way the
		 * neighbour runs decides which poses are offered; it has no bearing on what
		 * the piece rests on once placed.
		 *
		 * The contact normal is vertical, so the boxed inference classifies the
		 * joint before orientation is consulted — Bed (full mortar) for masonry,
		 * DryStone for timber.
		 */
		auto RestingJointsAtPose =
			[&NearbyBoxes, &NearbyMaterials, &Placed, &PlacedMaterial, &Settings](
				const FVector& Pose) -> TArray<FFormedJoint>
		{
			TArray<FFormedJoint> Resting;
			for (int32 j = 0; j < NearbyBoxes.Num(); ++j)
			{
				const DestructionLayout::FPieceBox& Support = NearbyBoxes[j];
				if (!IsBrickSized(Support.ExtentCm, Settings.BrickSizeCm))
				{
					continue;
				}

				const bool bOverlapX =
					FMath::Abs(Pose.X - Support.CentreCm.X)
						< Placed.ExtentCm.X + Support.ExtentCm.X;
				const bool bOverlapY =
					FMath::Abs(Pose.Y - Support.CentreCm.Y)
						< Placed.ExtentCm.Y + Support.ExtentCm.Y;
				const double GapZ =
					(Pose.Z - Placed.ExtentCm.Z) - (Support.CentreCm.Z + Support.ExtentCm.Z);
				const bool bRestsOn =
					FMath::Abs(GapZ - Settings.JointThicknessCm) < KINDA_SMALL_NUMBER;

				if (bOverlapX && bOverlapY && bRestsOn)
				{
					Resting.Add(FFormedJoint{
						j,
						JointForContact(
							PlacedMaterial,
							NearbyMaterials[j],
							FVector(0.0, 0.0, 1.0),
							DestructionLayout::FPieceBox{ Pose, Placed.ExtentCm },
							Support) });
				}
			}
			return Resting;
		};

		for (int32 i = 0; i < NearbyBoxes.Num(); ++i)
		{
			const DestructionLayout::FPieceBox& Other = NearbyBoxes[i];
			if (!IsBrickSized(Other.ExtentCm, Settings.BrickSizeCm))
			{
				continue;
			}

			/*
			 * The axis this neighbour runs along, and whether the placed piece is laid
			 * the same way. Running bond is a property of pieces that run together: a
			 * brick turned across its neighbour neither beds nor abuts end to end — it
			 * returns a corner instead (the other branch below).
			 */
			const FVector NeighbourAxis = LongAxisUnit(Other.ExtentCm);
			const FVector PlacedAxis = LongAxisUnit(Placed.ExtentCm);
			const bool bSameOrientation = NeighbourAxis.Equals(PlacedAxis, KINDA_SMALL_NUMBER);

			/*
			 * Which end of the neighbour the cursor leans toward, measured along the
			 * neighbour's length rather than X, so the offered pose is nearest where
			 * the player is pointing.
			 */
			const double AlongSign =
				(FVector::DotProduct(Placed.CentreCm - Other.CentreCm, NeighbourAxis) >= 0.0)
					? 1.0
					: -1.0;

			if (bPlacedIsBrick && bSameOrientation)
			{
				/*
				 * Next course up: half a brick along the neighbour's length so head joints
				 * stagger, one course up so the placed brick beds on this one.
				 *
				 * The beds are swept, not assumed: this neighbour decides the pose, but the
				 * brick then beds on whatever its underside rests on, which at a corner
				 * includes the return laid across the leg. Running bond straddles two bricks
				 * anyway, so a straight wall gains no joint it didn't already have.
				 */
				const double CoursePitchZ =
					Other.ExtentCm.Z + Settings.JointThicknessCm + Placed.ExtentCm.Z;
				const FVector NextCourseCentre =
					Other.CentreCm
					+ NeighbourAxis * (AlongSign * HalfStaggerCm)
					+ FVector(0.0, 0.0, CoursePitchZ);
				EmitOrMerge(
					ESnapKind::BrickNextCourse,
					NextCourseCentre,
					RestingJointsAtPose(NextCourseCentre));

				/*
				 * Same course, end to end: a full pitch along the neighbour's length at the
				 * same height. The shared face is an end face, so the normal is horizontal
				 * and the pieces run the same way — a head joint, so the inference returns
				 * the weak perpend, not bed mortar.
				 */
				const FVector SameCourseCentre =
					Other.CentreCm + NeighbourAxis * (AlongSign * SameCoursePitchCm);
				EmitOrMerge(
					ESnapKind::BrickSameCourse,
					SameCourseCentre,
					{ FFormedJoint{
						i,
						JointForContact(
							PlacedMaterial,
							NearbyMaterials[i],
							NeighbourAxis,
							DestructionLayout::FPieceBox{ SameCourseCentre, Placed.ExtentCm },
							Other) } });
			}

			if (bPlacedIsBrick && !bSameOrientation)
			{
				/*
				 * The corner return — the quoin of DESIGN §8's 2026-09-15 ruling. A brick
				 * laid across its neighbour's line turns the wall: it abuts one of the
				 * neighbour's end faces across one joint, and its own outer end face
				 * finishes flush with one of the neighbour's two width faces, reading as an
				 * L. Both ends and both width faces are offered — four poses per neighbour —
				 * ranked by distance to the cursor.
				 *
				 * The abutting offset is measured along the neighbour's length: its
				 * half-length out to the end face, one joint, then the return's own
				 * half-width along that same axis.
				 */
				const double EndOffsetCm =
					FVector::DotProduct(Other.ExtentCm, NeighbourAxis)
					+ Settings.JointThicknessCm
					+ FVector::DotProduct(Placed.ExtentCm, NeighbourAxis);

				/*
				 * Flush, measured along the return's length (the neighbour's width axis): the
				 * return's end face lands on the neighbour's width face, so its centre sits
				 * its own half-length inboard. The two signs are the neighbour's two width
				 * faces; the offset is negative for a real brick — the return is longer than
				 * the neighbour is wide, the overhang that makes the L.
				 */
				const double FlushOffsetCm =
					FVector::DotProduct(Other.ExtentCm, PlacedAxis)
					- FVector::DotProduct(Placed.ExtentCm, PlacedAxis);

				for (const double EndSign : { 1.0, -1.0 })
				{
					for (const double FaceSign : { 1.0, -1.0 })
					{
						const FVector ReturnCentre =
							Other.CentreCm
							+ NeighbourAxis * (EndSign * EndOffsetCm)
							+ PlacedAxis * (FaceSign * FlushOffsetCm);

						/*
						 * The interface normal points out of the neighbour's end face, so it is
						 * horizontal — the normal the three-argument inference answers with the
						 * weak perpend. Only the boxed overload, reading the crossed footprints,
						 * returns the quoin's full mortar here.
						 */
						EmitOrMerge(
							ESnapKind::BrickCornerReturn,
							ReturnCentre,
							{ FFormedJoint{
								i,
								JointForContact(
									PlacedMaterial,
									NearbyMaterials[i],
									NeighbourAxis * EndSign,
									DestructionLayout::FPieceBox{ ReturnCentre, Placed.ExtentCm },
									Other) } });
					}
				}
			}

			if (bPlacedIsTimber)
			{
				// Edge-flush is X-faces only, so the timber branch keeps its own X-side sign.
				const double SignX = (Placed.CentreCm.X >= Other.CentreCm.X) ? 1.0 : -1.0;

				// A plank rests one joint above the support top: brick top + one joint + placed half-height.
				const double BearZ =
					Other.CentreCm.Z + Other.ExtentCm.Z + Settings.JointThicknessCm + Placed.ExtentCm.Z;

				// Centred: the horizontal centre snaps to the brick's centre — a plank rests across its support rather than bonding into the pattern.
				const FVector CentredCentre(Other.CentreCm.X, Other.CentreCm.Y, BearZ);
				EmitOrMerge(
					ESnapKind::TimberCentered, CentredCentre, RestingJointsAtPose(CentredCentre));

				/*
				 * Edge-flush: the plank's near X-face aligns with the brick's near X-face
				 * instead of centring, on whichever side the cursor leans, so a plank pushed
				 * to one end of a beam finishes flush there. The +X face's plank centre sits
				 * its own half-width inboard; the -X face mirrors it.
				 */
				const double BrickXFace = Other.CentreCm.X + SignX * Other.ExtentCm.X;
				const FVector EdgeFlushCentre(
					BrickXFace - SignX * Placed.ExtentCm.X, Other.CentreCm.Y, BearZ);
				EmitOrMerge(
					ESnapKind::TimberEdgeFlush, EdgeFlushCentre, RestingJointsAtPose(EdgeFlushCentre));
			}
		}

		// Nearest snap first; equal offsets keep their relative order (stable sort).
		Candidates.StableSort(
			[](const FSnapCandidate& A, const FSnapCandidate& B)
			{
				return A.OffsetFromRequestedCm < B.OffsetFromRequestedCm;
			});

		/*
		 * Free fallback last: place exactly where requested, forming no joint. Not
		 * occupancy-filtered — the requested pose is the caller's explicit ask, and
		 * an existing fixture may place it deliberately touching a neighbour. Only
		 * the auto-generated snap poses are dropped for occupancy.
		 */
		Candidates.Add(FSnapCandidate{ ESnapKind::Free, Placed.CentreCm, 0.0, {} });
		return Candidates;
	}
}
