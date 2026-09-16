// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/BuildMode/SnapSolver.h"
#include "Core/BuildMode/JointInference.h"

namespace BuildMode
{
	namespace
	{
		/*
		 * A box is brick-sized when its FULL dimensions (twice the half-extent) match
		 * the coordinating brick within a loose centimetre tolerance, IN EITHER IN-PLANE
		 * ORIENTATION. The same brick turned through 90 degrees to run along Y is not a
		 * different piece — it is exactly what a corner return and a Y-running wall are
		 * built from — so the X/Y-swapped footprint passes the gate too; rejecting it is
		 * what left a rotated brick with nothing but the Free fallback. The gate still
		 * keeps the running-bond snaps from firing for timber or other non-brick pieces,
		 * which later behaviors handle with their own snap kinds.
		 */
		bool IsBrickSized(const FVector& ExtentCm, const FVector& BrickSizeCm)
		{
			const FVector FullCm = ExtentCm * 2.0;
			const FVector RotatedBrickCm(BrickSizeCm.Y, BrickSizeCm.X, BrickSizeCm.Z);
			return FullCm.Equals(BrickSizeCm, 0.5) || FullCm.Equals(RotatedBrickCm, 0.5);
		}

		/*
		 * Which in-plane axis a brick-sized box RUNS ALONG, as a unit vector. The running
		 * bond steps along the neighbour's LENGTH whichever way that wall is turned, so
		 * the grid is expressed in this axis rather than in X — an X-only grid cannot grow
		 * a wall along Y at all, which is half of what a corner is for.
		 *
		 * Only ever asked of a box that has already passed IsBrickSized, whose two in-plane
		 * dimensions therefore differ by a whole brick's width, so the comparison is never
		 * close. It is written `!(Y > X)` rather than `X >= Y` so that a NaN extent lands on
		 * X rather than on whichever branch the comparison order happened to favour.
		 */
		FVector LongAxisUnit(const FVector& ExtentCm)
		{
			return !(ExtentCm.Y > ExtentCm.X) ? FVector(1.0, 0.0, 0.0) : FVector(0.0, 1.0, 0.0);
		}

		/*
		 * Whether a placed box at Pose (with half-extent PlacedExtentCm) shares VOLUME with
		 * any nearby box. Interpenetration is a strict overlap on ALL THREE axes at once:
		 * Abs(dCentre) < sum-of-half-extents on X and Y and Z. The strict < is the whole
		 * distinction from a legitimate joint contact, which is GAPPED (or exactly touching)
		 * on one axis - a next-course pose clears in Z (7.5 > 6.5), a same-course pose clears
		 * in X (22.5 > 21.5) - so it overlaps on only two axes and is NOT an interpenetration.
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
	 * BEHAVIOR 2a — brick-on-brick RUNNING-BOND NEXT COURSE, plus the Free fallback.
	 *
	 * For each brick-sized nearby piece the placed brick could rest on, offer the
	 * next-course running-bond pose: ONE COURSE UP and HALF A BRICK ACROSS. Up,
	 * because a brick beds on the one below; half across, because running bond
	 * staggers alternate courses so head joints never line up. The stagger is taken
	 * on the +X or -X side the requested cursor leans toward, so the offered pose is
	 * the one nearest where the player is pointing.
	 *
	 * The bed joint's profile is INFERRED, never named here: JointForContact is fed a
	 * vertical (+Z) contact normal so it returns the strong bed mortar rather than a
	 * weak perpend.
	 *
	 * A brick laid in running bond STRADDLES the two below it, bedding onto both, so
	 * per-neighbour poses that coincide are coalesced into ONE candidate carrying both
	 * bed joints. Snaps are then ordered nearest-first (ascending offset from the
	 * requested pose) and the Free fallback appended last, so any in-range snap
	 * outranks placing the piece exactly where it was requested.
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
		 * What the PLACED piece is decides which snap kinds it can take. A brick-sized
		 * piece bonds into the running-bond grid (bed + head). A timber piece — not
		 * compression-dominant — does not bond; it BEARS, sitting centred on the support
		 * below rather than staggering into a course, so it gets the centred snap only.
		 */
		const bool bPlacedIsBrick = IsBrickSized(Placed.ExtentCm, Settings.BrickSizeCm);
		const bool bPlacedIsTimber = !PlacedMaterial.bCompressionDominant;

		/*
		 * Two sources that AGREE inside the brick-sized gate. The half-stagger across
		 * is half a coordinating brick pitch, taken from Settings (brick length + head
		 * joint); the course rise up is taken from the ACTUAL box half-heights plus the
		 * bed joint. For real bricks these match Layout's documented 22.5 x 11.25 x 7.5
		 * grid — the gate is what guarantees the two boxes are that brick.
		 *
		 * BOTH ARE LENGTHS OF A BRICK, NOT OF AN AXIS. BrickSizeCm.X names the brick's
		 * LENGTH, which is a property of the piece; the pitch it gives is stepped along
		 * whichever world axis the NEIGHBOUR happens to run along.
		 */
		const double HalfStaggerCm = (Settings.BrickSizeCm.X + Settings.JointThicknessCm) / 2.0;

		/*
		 * Same-course pitch: one whole brick length plus one head joint. End-to-end on
		 * the same course, so the pose is a full pitch along the neighbour's length at the
		 * SAME height, not a course up.
		 */
		const double SameCoursePitchCm = Settings.BrickSizeCm.X + Settings.JointThicknessCm;

		/*
		 * Emit a snap at Centre, or merge into a coincident one. A brick laid in running
		 * bond STRADDLES the two below it, so per-neighbour poses that coincide are
		 * coalesced into ONE candidate carrying every bed joint; the same union serves
		 * any other kind whose poses happen to meet. Poses beyond the snap radius are
		 * dropped.
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
			 * Drop a pose whose box would OCCUPY a cell another piece already fills. The
			 * placed half-extent is what interpenetrates, so it is measured, not the snap
			 * geometry; a joint contact is gapped on one axis and survives this.
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
				 * Union by OtherPieceIndex. A placed piece forms at most one joint to any
				 * one neighbour at a single pose, so a second joint to an index already
				 * present is a true duplicate — as happens when a brick-width plank's
				 * centred and edge-flush poses coincide and the same bearing set merges
				 * twice. Distinct indices (the brick straddle, the bed+head pair, a plate's
				 * separate bearings) carry different neighbours and are all kept.
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
		 * Every joint the placed piece forms by RESTING at Pose, found by CONTACT rather
		 * than by which neighbour happened to fix the pose: a brick-sized piece j qualifies
		 * when the placed box positively overlaps it in both X and Y and its underside sits
		 * exactly one joint above j's top face.
		 *
		 * WHY CONTACT AND NOT THE POSE'S OWN NEIGHBOUR. A plank SPANS — it rests on every
		 * brick beneath it, not only the one it was centred on. A brick in running bond
		 * STRADDLES two below it, and at a corner one of those two is the RETURN laid
		 * ACROSS the leg: the alternate-course lap that makes a quoin carry load from above
		 * (DESIGN §8's 2026-09-15 corner ruling, review finding B2). Which way the neighbour
		 * runs decides which POSES are offered; it has no bearing on what the piece is
		 * sitting on once it is there.
		 *
		 * The contact normal is vertical, so the boxed inference classifies the joint before
		 * orientation is ever consulted — a Bed (full mortar) for masonry, and DryStone for
		 * timber, whose face is not compression-dominant.
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
			 * The axis this neighbour RUNS ALONG, and whether the placed piece is laid the
			 * same way. Running bond is a property of pieces that run TOGETHER: a brick
			 * turned across its neighbour neither beds half a brick along it nor abuts it end
			 * to end — it returns a corner, which is the other branch below.
			 */
			const FVector NeighbourAxis = LongAxisUnit(Other.ExtentCm);
			const FVector PlacedAxis = LongAxisUnit(Placed.ExtentCm);
			const bool bSameOrientation = NeighbourAxis.Equals(PlacedAxis, KINDA_SMALL_NUMBER);

			/*
			 * Which end of the neighbour the cursor leans toward, measured ALONG THE
			 * NEIGHBOUR'S LENGTH rather than along X, so the offered pose is the one nearest
			 * where the player is pointing whichever way the wall runs.
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
				 * THE BEDS ARE SWEPT, NOT ASSUMED. This neighbour is what decides the POSE —
				 * the grid steps along a piece laid the same way — but the brick then beds on
				 * WHATEVER its underside has come to rest on, which at a corner includes the
				 * return laid ACROSS the leg. Running bond straddles two bricks anyway, so the
				 * sweep is what the merge of two neighbours' poses was already approximating;
				 * a collinear neighbour underneath is in the swept list too, so a straight wall
				 * gains no joint it did not already have.
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
				 * same height. The shared face is an END face, so the interface normal is
				 * HORIZONTAL and the two pieces run the same way — a head joint, so the boxed
				 * inference returns the WEAK perpend, not the bed mortar and not a quoin's.
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
				 * THE CORNER RETURN — the quoin of DESIGN §8's 2026-09-15 ruling. A brick laid
				 * ACROSS its neighbour's line turns the wall: it abuts one of the neighbour's
				 * END faces across one joint, and its own outer end face finishes FLUSH with
				 * one of the neighbour's two WIDTH faces, so the pair reads as an L. Both ends
				 * and both width faces are offered — four poses per neighbour — and ranking by
				 * distance picks the one the cursor is nearest.
				 *
				 * The abutting offset is measured along the NEIGHBOUR's length: its half-length
				 * out to the end face, one joint, then the return's own half-WIDTH (which is
				 * its extent along that same axis, the return running across it).
				 */
				const double EndOffsetCm =
					FVector::DotProduct(Other.ExtentCm, NeighbourAxis)
					+ Settings.JointThicknessCm
					+ FVector::DotProduct(Placed.ExtentCm, NeighbourAxis);

				/*
				 * Flush, measured along the RETURN's length (the neighbour's width axis): the
				 * return's end face lands on the neighbour's width face, so the return's centre
				 * sits its own half-length inboard of it. The two signs are the neighbour's two
				 * width faces, and the offset is negative for a real brick — the return is
				 * longer than the neighbour is wide — which is exactly the overhang that makes
				 * the L.
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
						 * HORIZONTAL — precisely the normal the three-argument inference answers
						 * with the weak perpend. Only the BOXED overload, which reads the two
						 * footprints and sees their long axes crossed, returns the quoin's full
						 * mortar here.
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
				// Edge-flush is X-FACES ONLY, so the timber branch keeps its own X-side sign.
				const double SignX = (Placed.CentreCm.X >= Other.CentreCm.X) ? 1.0 : -1.0;

				/*
				 * A plank rests one joint above the support top, keyed on the PLACED piece's
				 * own half-thickness — brick top + one joint + placed half-height.
				 */
				const double BearZ =
					Other.CentreCm.Z + Other.ExtentCm.Z + Settings.JointThicknessCm + Placed.ExtentCm.Z;

				/*
				 * Centred: the horizontal centre snaps to the brick's centre — a plank does
				 * not bond into the bond pattern, it rests across its support.
				 */
				const FVector CentredCentre(Other.CentreCm.X, Other.CentreCm.Y, BearZ);
				EmitOrMerge(
					ESnapKind::TimberCentered, CentredCentre, RestingJointsAtPose(CentredCentre));

				/*
				 * Edge-flush: the plank's NEAR X-face aligns with the brick's near X-face
				 * instead of centring. Pick the face on the side the cursor leans toward, so
				 * a plank pushed to one end of a beam finishes flush there. For the +X face
				 * the plank centre sits its own half-width inboard of that face; the -X face
				 * mirrors it.
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
		 * Free fallback last: place exactly where requested, forming no joint. It is NOT
		 * occupancy-filtered - the raw requested pose is the caller's explicit ask and the
		 * last resort when nothing snaps, and an existing fixture places it deliberately
		 * touching a neighbour. Only the auto-generated SNAP poses are dropped for occupancy,
		 * since those are the ones the solver invents.
		 */
		Candidates.Add(FSnapCandidate{ ESnapKind::Free, Placed.CentreCm, 0.0, {} });
		return Candidates;
	}
}
