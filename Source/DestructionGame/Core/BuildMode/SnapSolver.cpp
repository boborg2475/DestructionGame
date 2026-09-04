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

		if (!IsBrickSized(Placed.ExtentCm, Settings.BrickSizeCm))
		{
			Candidates.Add(FSnapCandidate{ ESnapKind::Free, Placed.CentreCm, 0.0, {} });
			return Candidates;
		}

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
				ESnapKind Kind, const FVector& Centre, const FFormedJoint& Joint)
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
				Existing->Joints.Add(Joint);
				return;
			}

			FSnapCandidate Candidate;
			Candidate.Kind = Kind;
			Candidate.CentreCm = Centre;
			Candidate.OffsetFromRequestedCm = Offset;
			Candidate.Joints.Add(Joint);
			Candidates.Add(MoveTemp(Candidate));
		};

		for (int32 i = 0; i < NearbyBoxes.Num(); ++i)
		{
			const DestructionLayout::FPieceBox& Other = NearbyBoxes[i];
			if (!IsBrickSized(Other.ExtentCm, Settings.BrickSizeCm))
			{
				continue;
			}

			const double SignX = (Placed.CentreCm.X >= Other.CentreCm.X) ? 1.0 : -1.0;

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
				FFormedJoint{
					i,
					JointForContact(PlacedMaterial, NearbyMaterials[i], FVector(0.0, 0.0, 1.0)) });

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
				FFormedJoint{
					i,
					JointForContact(PlacedMaterial, NearbyMaterials[i], FVector(1.0, 0.0, 0.0)) });
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
