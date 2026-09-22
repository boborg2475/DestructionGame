// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/BuildMode/SnapSolver.h"
#include "Core/BuildMode/JointInference.h"

namespace BuildMode
{
	namespace
	{
		/*
		 * Whether a box matches the coordinating brick within 0.5 cm, in either in-plane
		 * orientation, since a brick turned to run along Y is still a brick.
		 */
		bool IsBrickSized(const FVector& ExtentCm, const FVector& BrickSizeCm)
		{
			const FVector FullCm = ExtentCm * 2.0;
			const FVector RotatedBrickCm(BrickSizeCm.Y, BrickSizeCm.X, BrickSizeCm.Z);
			return FullCm.Equals(BrickSizeCm, 0.5) || FullCm.Equals(RotatedBrickCm, 0.5);
		}

		/*
		 * The in-plane axis a brick-sized box runs along, so the bond grid can grow along Y as
		 * well as X. Written `!(Y > X)` so a NaN extent lands on X.
		 */
		FVector LongAxisUnit(const FVector& ExtentCm)
		{
			return !(ExtentCm.Y > ExtentCm.X) ? FVector(1.0, 0.0, 0.0) : FVector(0.0, 1.0, 0.0);
		}

		/*
		 * Whether the box at Pose strictly overlaps any nearby box on all three axes. Strict <
		 * lets a joint contact through, since it is gapped or touching on one axis.
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
	 * Snap poses against each nearby piece (next course, same course, corner return, timber
	 * bearing), nearest first, with Free appended last. Joint profiles come from JointForContact.
	 * Coincident poses merge into one candidate carrying every joint.
	 */
	TArray<FSnapCandidate> SolveSnapCandidates(
		const DestructionLayout::FPieceBox& Placed,
		const DestructionProfiles::FMaterialProfile& PlacedMaterial,
		TArrayView<const DestructionLayout::FPieceBox> NearbyBoxes,
		TArrayView<const DestructionProfiles::FMaterialProfile> NearbyMaterials,
		const FSnapSettings& Settings)
	{
		TArray<FSnapCandidate> Candidates;

		// Bricks bond into the running-bond grid; timber (not compression-dominant) bears on its support.
		const bool bPlacedIsBrick = IsBrickSized(Placed.ExtentCm, Settings.BrickSizeCm);
		const bool bPlacedIsTimber = !PlacedMaterial.bCompressionDominant;

		/*
		 * BrickSizeCm.X is the brick's length, stepped along whichever axis the neighbour runs.
		 * The course rise comes from the actual box half-heights instead.
		 */
		const double HalfStaggerCm = (Settings.BrickSizeCm.X + Settings.JointThicknessCm) / 2.0;

		const double SameCoursePitchCm = Settings.BrickSizeCm.X + Settings.JointThicknessCm;

		/*
		 * Adds a snap at Centre, or merges its joints into a coincident one (a running-bond brick
		 * straddles two below). Drops poses beyond the snap radius.
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
				// Union by OtherPieceIndex: at one pose there is at most one joint per neighbour.
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
		 * Every bed joint formed by resting at Pose, found by contact (XY overlap, underside one
		 * joint above the top face) rather than by the neighbour that fixed the pose. A plank
		 * spans several bricks and a running-bond brick straddles two, one of which may be a
		 * corner return (DESIGN §8, 2026-09-15). The vertical normal gives Bed for masonry and
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

			// A brick laid across its neighbour returns a corner instead of bonding.
			const FVector NeighbourAxis = LongAxisUnit(Other.ExtentCm);
			const FVector PlacedAxis = LongAxisUnit(Placed.ExtentCm);
			const bool bSameOrientation = NeighbourAxis.Equals(PlacedAxis, KINDA_SMALL_NUMBER);

			// Which end of the neighbour the cursor leans toward, along its length.
			const double AlongSign =
				(FVector::DotProduct(Placed.CentreCm - Other.CentreCm, NeighbourAxis) >= 0.0)
					? 1.0
					: -1.0;

			if (bPlacedIsBrick && bSameOrientation)
			{
				/*
				 * Next course: half a brick along and one course up. Beds are swept by contact, so a
				 * corner return underneath is included.
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

				// Same course, end to end: a horizontal normal, so the inference gives a perpend.
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
				 * Corner return (DESIGN §8, 2026-09-15): abuts one of the neighbour's end faces across
				 * a joint, with its outer end flush to one of the neighbour's width faces. Four poses:
				 * two ends by two faces.
				 */
				const double EndOffsetCm =
					FVector::DotProduct(Other.ExtentCm, NeighbourAxis)
					+ Settings.JointThicknessCm
					+ FVector::DotProduct(Placed.ExtentCm, NeighbourAxis);

				// Along the return's length; negative for a real brick, which is longer than the neighbour is wide.
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

						// Horizontal normal; only the boxed overload sees the crossed footprints and returns full mortar.
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
				// Edge-flush uses X faces only.
				const double SignX = (Placed.CentreCm.X >= Other.CentreCm.X) ? 1.0 : -1.0;

				const double BearZ =
					Other.CentreCm.Z + Other.ExtentCm.Z + Settings.JointThicknessCm + Placed.ExtentCm.Z;

				const FVector CentredCentre(Other.CentreCm.X, Other.CentreCm.Y, BearZ);
				EmitOrMerge(
					ESnapKind::TimberCentered, CentredCentre, RestingJointsAtPose(CentredCentre));

				// The plank's near X face aligns with the brick's, on the side the cursor leans.
				const double BrickXFace = Other.CentreCm.X + SignX * Other.ExtentCm.X;
				const FVector EdgeFlushCentre(
					BrickXFace - SignX * Placed.ExtentCm.X, Other.CentreCm.Y, BearZ);
				EmitOrMerge(
					ESnapKind::TimberEdgeFlush, EdgeFlushCentre, RestingJointsAtPose(EdgeFlushCentre));
			}
		}

		Candidates.StableSort(
			[](const FSnapCandidate& A, const FSnapCandidate& B)
			{
				return A.OffsetFromRequestedCm < B.OffsetFromRequestedCm;
			});

		// Free: exactly where requested, no joints. Not occupancy-filtered, since it is the caller's explicit ask.
		Candidates.Add(FSnapCandidate{ ESnapKind::Free, Placed.CentreCm, 0.0, {} });
		return Candidates;
	}
}
