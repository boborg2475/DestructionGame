// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/BuildMode/SnapSolver.h"
#include "Core/BuildMode/JointInference.h"

namespace BuildMode
{
	namespace
	{
		/*
		 * A box is brick-sized when its FULL dimensions (twice the half-extent) match
		 * the coordinating brick within a loose centimetre tolerance. This gate keeps
		 * the running-bond next-course snap from firing for timber or other non-brick
		 * pieces, which later behaviors handle with their own snap kinds.
		 */
		bool IsBrickSized(const FVector& ExtentCm, const FVector& BrickSizeCm)
		{
			return (ExtentCm * 2.0).Equals(BrickSizeCm, 0.5);
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
		 */
		const double HalfStaggerX = (Settings.BrickSizeCm.X + Settings.JointThicknessCm) / 2.0;

		/*
		 * Same-course pitch: one whole brick length plus one head joint. End-to-end on
		 * the same course, so the pose is a full pitch across at the SAME Y and Z, not
		 * a course up.
		 */
		const double SameCoursePitchX = Settings.BrickSizeCm.X + Settings.JointThicknessCm;

		/*
		 * Emit a snap at Centre, or merge into a coincident one. A brick laid in running
		 * bond STRADDLES the two below it, so per-neighbour poses that coincide are
		 * coalesced into ONE candidate carrying every bed joint; the same union serves
		 * any other kind whose poses happen to meet. Poses beyond the snap radius are
		 * dropped.
		 */
		auto EmitOrMerge =
			[&Candidates, &Placed, &Settings](
				ESnapKind Kind, const FVector& Centre, const TArray<FFormedJoint>& Joints)
		{
			const double Offset = (Centre - Placed.CentreCm).Size();
			if (Offset > Settings.SnapRadiusCm)
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
		 * The bearings a timber piece forms when placed at Pose. A plank SPANS: it rests
		 * on EVERY brick beneath it, not only the one that fixed its pose, so its bearings
		 * are found by CONTACT — a brick j qualifies when the timber positively overlaps
		 * it in both X and Y and its underside sits exactly one joint above j's top face.
		 * Each is a passive DryStone bearing; JointForContact returns DryStone because the
		 * timber face is not compression-dominant, whatever the normal. Both timber snap
		 * kinds (centred, edge-flush) differ only in their pose and share this.
		 */
		auto BearingsAtPose =
			[&NearbyBoxes, &NearbyMaterials, &Placed, &PlacedMaterial, &Settings](
				const FVector& Pose) -> TArray<FFormedJoint>
		{
			TArray<FFormedJoint> Bearings;
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
					Bearings.Add(FFormedJoint{
						j,
						JointForContact(PlacedMaterial, NearbyMaterials[j], FVector(0.0, 0.0, 1.0)) });
				}
			}
			return Bearings;
		};

		for (int32 i = 0; i < NearbyBoxes.Num(); ++i)
		{
			const DestructionLayout::FPieceBox& Other = NearbyBoxes[i];
			if (!IsBrickSized(Other.ExtentCm, Settings.BrickSizeCm))
			{
				continue;
			}

			const double SignX = (Placed.CentreCm.X >= Other.CentreCm.X) ? 1.0 : -1.0;

			if (bPlacedIsBrick)
			{
				/*
				 * Next course up: half a brick across so head joints stagger, one course up
				 * so the placed brick beds on this one. The shared face is horizontal, so the
				 * interface normal is +Z and JointForContact returns the strong bed mortar.
				 */
				const double CoursePitchZ =
					Other.ExtentCm.Z + Settings.JointThicknessCm + Placed.ExtentCm.Z;
				const FVector NextCourseCentre =
					Other.CentreCm + FVector(SignX * HalfStaggerX, 0.0, CoursePitchZ);
				EmitOrMerge(
					ESnapKind::BrickNextCourse,
					NextCourseCentre,
					{ FFormedJoint{
						i,
						JointForContact(PlacedMaterial, NearbyMaterials[i], FVector(0.0, 0.0, 1.0)) } });

				/*
				 * Same course, end to end: a full pitch across at the SAME Y and Z. The
				 * shared face is an END face, so the interface normal is horizontal (+/-X)
				 * and JointForContact returns the WEAK perpend, not the bed mortar.
				 */
				const FVector SameCourseCentre =
					Other.CentreCm + FVector(SignX * SameCoursePitchX, 0.0, 0.0);
				EmitOrMerge(
					ESnapKind::BrickSameCourse,
					SameCourseCentre,
					{ FFormedJoint{
						i,
						JointForContact(PlacedMaterial, NearbyMaterials[i], FVector(1.0, 0.0, 0.0)) } });
			}

			if (bPlacedIsTimber)
			{
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
					ESnapKind::TimberCentered, CentredCentre, BearingsAtPose(CentredCentre));

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
					ESnapKind::TimberEdgeFlush, EdgeFlushCentre, BearingsAtPose(EdgeFlushCentre));
			}
		}

		// Nearest snap first; equal offsets keep their relative order (stable sort).
		Candidates.StableSort(
			[](const FSnapCandidate& A, const FSnapCandidate& B)
			{
				return A.OffsetFromRequestedCm < B.OffsetFromRequestedCm;
			});

		// Free fallback last: place exactly where requested, forming no joint.
		Candidates.Add(FSnapCandidate{ ESnapKind::Free, Placed.CentreCm, 0.0, {} });
		return Candidates;
	}
}
