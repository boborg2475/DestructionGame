// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/SessionToolbar.h"
#include "Core/Structure.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/DestructionStructureSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Uniquely named namespace: unity builds merge files, so anonymous-namespace names can collide. */
namespace BuildJointOverrideTestSupport
{
	/*
	 * Running-bond grid, transcribed not imported (DESIGN §8): a 21.5 x 10.25 x 6.5 cm brick on 1 cm
	 * joints gives a 22.5 x 11.25 x 7.5 grid, and course 0 centres at half height (3.25) so the
	 * brick rests on the ground. A retune then fails a fixture row instead of silently agreeing.
	 */
	const FVector OverrideHalfBrickCm(10.75, 5.125, 3.25);

	/** The demo building's timber wall plate, half sizes (67.5 cm long). */
	const FVector OverrideHalfPlateCm(33.75, 5.125, 5.0);

	/** Course 0's centre for a brick: half a brick above the earth. */
	constexpr double OverrideCourseZeroBrickZCm = 3.25;

	/** The seed brick, and the same-course pose one 22.5 cm pitch along. */
	const FVector OverrideSeedCentreCm(0.0, 0.0, OverrideCourseZeroBrickZCm);
	const FVector OverrideSameCourseCentreCm(22.5, 0.0, OverrideCourseZeroBrickZCm);

	/**
	 * Cursor for the second brick, 1.5 cm short of the bond, so landing at 22.5 proves the snap.
	 * The next-course pose is 12.3 cm away, so the same-course snap wins by a factor of eight.
	 */
	const FVector OverrideSecondCursorCm(21.0, 0.0, OverrideCourseZeroBrickZCm);

	/** Head joint area: the shared Y-Z end face, 10.25 x 6.5 = 66.625 cm2. */
	constexpr double OverrideHeadAreaSqCm = 66.625;

	/** Plate centre on the course: brick top 6.5, plus 1 cm joint, plus 5 cm half height = 12.5. */
	constexpr double OverridePlateBearingZCm = 12.5;
	const FVector OverridePlateCentreCm(0.0, 0.0, OverridePlateBearingZCm);

	/** Each bearing's area: the plate covers each brick fully, so 21.5 x 10.25 = 220.375 cm2. */
	constexpr double OverrideBearingAreaSqCm = 220.375;

	/**
	 * Compare all five profile fields (FConnectionStrength has no operator==). Library siblings
	 * differ in only one or two fields, so checking one would let a sibling through.
	 */
	void CheckOverrideProfile(
		FAutomationTestBase& Test,
		const FString& Prefix,
		const FConnectionStrength& Got,
		const FConnectionStrength& Want)
	{
		Test.TestEqual(Prefix + TEXT("CompressiveStrengthMPa"),
			Got.CompressiveStrengthMPa, Want.CompressiveStrengthMPa);
		Test.TestEqual(Prefix + TEXT("ShearCohesionMPa"),
			Got.ShearCohesionMPa, Want.ShearCohesionMPa);
		Test.TestEqual(Prefix + TEXT("TensileStrengthMPa"),
			Got.TensileStrengthMPa, Want.TensileStrengthMPa);
		Test.TestEqual(Prefix + TEXT("FrictionCoefficient"),
			Got.FrictionCoefficient, Want.FrictionCoefficient);
		Test.TestEqual(Prefix + TEXT("MaxShearStrengthMPa"),
			Got.MaxShearStrengthMPa, Want.MaxShearStrengthMPa);
	}

	/** One joint's geometry, for comparing two runs. */
	struct FJointShape
	{
		int32 PieceA = INDEX_NONE;
		int32 PieceB = INDEX_NONE;
		double AreaSqCm = 0.0;
		FVector NormalUnit = FVector::ZeroVector;
	};

	FJointShape ShapeOf(const FConnection& Conn)
	{
		FJointShape Shape;
		Shape.PieceA = Conn.PieceA;
		Shape.PieceB = Conn.PieceB;
		Shape.AreaSqCm = Conn.InterfaceAreaSqCm;
		Shape.NormalUnit = Conn.InterfaceNormal;
		return Shape;
	}

	FString DescribeShape(const FJointShape& Shape)
	{
		return FString::Printf(
			TEXT("{%d-%d, %g cm2, normal (%g, %g, %g)}"),
			Shape.PieceA, Shape.PieceB, Shape.AreaSqCm,
			Shape.NormalUnit.X, Shape.NormalUnit.Y, Shape.NormalUnit.Z);
	}

	bool ShapesAreTheSameJoint(const FJointShape& A, const FJointShape& B)
	{
		return A.PieceA == B.PieceA
			&& A.PieceB == B.PieceB
			&& A.AreaSqCm == B.AreaSqCm
			&& A.NormalUnit == B.NormalUnit;
	}

	/** Name of the shipped connection profile at this address (never compared by value). */
	const TCHAR* OverrideProfileName(const FConnectionStrength* Profile)
	{
		using namespace DestructionProfiles;

		if (Profile == nullptr)                      { return TEXT("<nullptr>"); }
		if (Profile == &GeneralPurposeMortar)        { return TEXT("GeneralPurposeMortar"); }
		if (Profile == &GeneralPurposeMortarPerpend) { return TEXT("GeneralPurposeMortarPerpend"); }
		if (Profile == &LimeMortar)                  { return TEXT("LimeMortar"); }
		if (Profile == &DryStone)                    { return TEXT("DryStone"); }
		if (Profile == &Nail)                        { return TEXT("Nail"); }
		if (Profile == &Screw)                       { return TEXT("Screw"); }
		if (Profile == &Bolt)                        { return TEXT("Bolt"); }

		return TEXT("<not a shipped library row>");
	}

	/**
	 * Open a build and lay one grounded brick, Auto. A fresh structure per case, so every override
	 * placement sees the same neighbourhood as the Auto run.
	 */
	int32 SeedOneBrick(UDestructionStructureSubsystem& Subsystem)
	{
		using namespace DestructionProfiles;

		const int32 StructureId = Subsystem.BeginBuild();

		Subsystem.PlaceBuildPiece(
			StructureId, OverrideSeedCentreCm, OverrideHalfBrickCm, ClayBrick);

		return StructureId;
	}

	/** A jointless pose: 500 cm along, far beyond the 30 cm snap radius, and placed Free. */
	const FVector OverrideLonelyCentreCm(500.0, 0.0, OverrideCourseZeroBrickZCm);

	/** Open a build and lay a TWO-brick grounded course into it, both Auto. */
	int32 SeedTwoBrickCourse(UDestructionStructureSubsystem& Subsystem)
	{
		using namespace DestructionProfiles;

		const int32 StructureId = SeedOneBrick(Subsystem);

		Subsystem.PlaceBuildPiece(
			StructureId, OverrideSecondCursorCm, OverrideHalfBrickCm, ClayBrick);

		return StructureId;
	}
}

/**
 * UI-6 world half: a joint-profile override passed to PreviewBuildPiece/PlaceBuildPiece replaces
 * the profile of every joint the placed piece forms, and changes nothing else. Null means the
 * inference (JointForContact) decides.
 *
 * A plate across two bricks forms two bearings, which catches an override applied only to the
 * first joint. Each override case is compared joint by joint against an identically seeded Auto
 * run, so a re-solve or a dropped joint fails. Poses are read from the recorded box, never from
 * displacement (DESIGN §4).
 *
 * Needs a real world (both doors spawn an ABrickActor) but it never ticks or solves.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildJointOverrideRidesThroughPlacementTest,
	"DestructionGame.World.BuildMode.JointOverrideRidesThroughPlacement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildJointOverrideRidesThroughPlacementTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildJointOverrideTestSupport;
	using namespace DestructionProfiles;
	using namespace DestructionSession;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		TestWorld.End();
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	// Overrides come from the toolbar's JointOverrideFor, the same path a chip click takes.
	const FConnectionStrength* const AutoOverride = JointOverrideFor(EJointChoice::Auto);
	const FConnectionStrength* const MortarOverride = JointOverrideFor(EJointChoice::Mortar);
	const FConnectionStrength* const DryOverride = JointOverrideFor(EJointChoice::Dry);
	const FConnectionStrength* const ScrewOverride = JointOverrideFor(EJointChoice::Screw);

	// One: the Auto baseline. A same-course brick gets the weak perpend.

	FJointShape AutoHeadShape;

	{
		const int32 StructureId = SeedOneBrick(Subsystem);

		const FBuildPreview Preview = Subsystem.PreviewBuildPiece(
			StructureId, OverrideSecondCursorCm, OverrideHalfBrickCm, ClayBrick,
			EPlacementMode::Snap, AutoOverride);

		TestTrue(TEXT("fixture: the Auto preview of the same-course brick must be valid"),
			Preview.bValid);

		TestEqual(
			FString::Printf(
				TEXT("the Auto same-course placement forms exactly one head joint, the preview says %d"),
				Preview.JointCount),
			Preview.JointCount, 1);

		/*
		 * JointProfile must be the shipped row's address: JointForContact returns by value, so a
		 * pointer into that result would dangle once the preview returns.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("with no override the preview must name the INFERRED profile, "
					 "GeneralPurposeMortarPerpend, by address — it named %s"),
				OverrideProfileName(Preview.JointProfile)),
			Preview.JointProfile == &GeneralPurposeMortarPerpend);

		const FPieceRef Ref = Subsystem.PlaceBuildPiece(
			StructureId, OverrideSecondCursorCm, OverrideHalfBrickCm, ClayBrick,
			EPlacementMode::Snap, AutoOverride);

		FStructureBinding* const Binding = Subsystem.Find(StructureId);

		if (Binding == nullptr || Ref.PieceIndex != 1 || Binding->GetStructure().NumConnections() != 1)
		{
			AddError(FString::Printf(
				TEXT("fixture: the Auto same-course brick must land as piece 1 with one joint; it "
					 "landed as %d with %d joints"),
				Ref.PieceIndex,
				Binding != nullptr ? Binding->GetStructure().NumConnections() : INDEX_NONE));

			TestWorld.End();
			return true;
		}

		const FVector CentreCm = Binding->GetBinding(1).Box.CentreCm;

		TestTrue(
			FString::Printf(
				TEXT("fixture: the cursor at x = 21 must be PULLED onto the bond at (22.5, 0, 3.25); "
					 "it landed at (%g, %g, %g)"),
				CentreCm.X, CentreCm.Y, CentreCm.Z),
			CentreCm.Equals(OverrideSameCourseCentreCm, KINDA_SMALL_NUMBER));

		const FConnection& Head = Binding->GetStructure().GetConnection(0);

		AutoHeadShape = ShapeOf(Head);

		// A horizontal normal makes it a head joint, which is why the perpend is inferred.
		TestEqual(
			FString::Printf(
				TEXT("fixture: a head joint's normal is horizontal, |X| should be 1, got %g"),
				FMath::Abs(Head.InterfaceNormal.X)),
			FMath::Abs(Head.InterfaceNormal.X), 1.0, 1.0e-9);

		TestEqual(
			FString::Printf(
				TEXT("fixture: the head face is 10.25 x 6.5 = 66.625 cm2, got %g"),
				Head.InterfaceAreaSqCm),
			Head.InterfaceAreaSqCm, OverrideHeadAreaSqCm, 1.0e-6);

		CheckOverrideProfile(
			*this,
			TEXT("AUTO baseline: the same-course joint is the inferred perpend: "),
			Head.Strength,
			GeneralPurposeMortarPerpend);
	}

	// Two: the same placement with each override, against that baseline.

	struct FBrickOverrideCase
	{
		const TCHAR* Description;
		const FConnectionStrength* Override;
		const FConnectionStrength* Expected;
	};

	const FBrickOverrideCase BrickCases[] = {
		{
			TEXT("MORTAR: the player asks for a full bed bond in a head joint the inference would "
				 "have weakened to a perpend — a wall that is stronger than a bonded one, which is "
				 "the whole reason the choice exists"),
			MortarOverride,
			&GeneralPurposeMortar,
		},
		{
			TEXT("DRY: the same two bricks laid with no bond at all, so the course holds by friction "
				 "and stands only while it is squeezed"),
			DryOverride,
			&DryStone,
		},
		{
			TEXT("SCREW: a fastener in a masonry head joint — physically odd and deliberately "
				 "allowed, because the override is the PLAYER's and the model must not quietly "
				 "second-guess which pairings are sensible"),
			ScrewOverride,
			&Screw,
		},
	};

	for (const FBrickOverrideCase& Case : BrickCases)
	{
		const int32 StructureId = SeedOneBrick(Subsystem);

		const FBuildPreview Preview = Subsystem.PreviewBuildPiece(
			StructureId, OverrideSecondCursorCm, OverrideHalfBrickCm, ClayBrick,
			EPlacementMode::Snap, Case.Override);

		TestEqual(
			FString::Printf(
				TEXT("%s: the override must not change the joint COUNT, the preview says %d against "
					 "the Auto run's 1"),
				Case.Description, Preview.JointCount),
			Preview.JointCount, 1);

		TestTrue(
			*FString::Printf(
				TEXT("%s: the preview must name the OVERRIDE, %s, by address — it named %s"),
				Case.Description, OverrideProfileName(Case.Expected),
				OverrideProfileName(Preview.JointProfile)),
			Preview.JointProfile == Case.Expected);

		Subsystem.PlaceBuildPiece(
			StructureId, OverrideSecondCursorCm, OverrideHalfBrickCm, ClayBrick,
			EPlacementMode::Snap, Case.Override);

		FStructureBinding* const Binding = Subsystem.Find(StructureId);

		if (Binding == nullptr || Binding->GetStructure().NumConnections() != 1)
		{
			AddError(FString::Printf(
				TEXT("%s: the overridden placement must still form exactly ONE joint, it formed %d"),
				Case.Description,
				Binding != nullptr ? Binding->GetStructure().NumConnections() : INDEX_NONE));

			continue;
		}

		const FConnection& Head = Binding->GetStructure().GetConnection(0);

		// Only the strength may differ; geometry must match the Auto run exactly.
		TestTrue(
			*FString::Printf(
				TEXT("%s: the joint's GEOMETRY must be the Auto run's exactly — Auto gave %s, this "
					 "gave %s"),
				Case.Description, *DescribeShape(AutoHeadShape), *DescribeShape(ShapeOf(Head))),
			ShapesAreTheSameJoint(ShapeOf(Head), AutoHeadShape));

		TestTrue(
			FString::Printf(
				TEXT("%s: and the pose must still be the bond's (22.5, 0, 3.25)"), Case.Description),
			Binding->GetBinding(1).Box.CentreCm.Equals(
				OverrideSameCourseCentreCm, KINDA_SMALL_NUMBER));

		CheckOverrideProfile(
			*this,
			FString::Printf(TEXT("%s: the committed joint carries the override: "), Case.Description),
			Head.Strength,
			*Case.Expected);
	}

	// Three: a plate bearing on two bricks, Auto baseline.

	TArray<FJointShape> AutoBearingShapes;

	{
		const int32 StructureId = SeedTwoBrickCourse(Subsystem);

		const FBuildPreview Preview = Subsystem.PreviewBuildPiece(
			StructureId, OverridePlateCentreCm, OverrideHalfPlateCm, Timber,
			EPlacementMode::Snap, AutoOverride);

		TestEqual(
			FString::Printf(
				TEXT("fixture: a 67.5 cm plate laid across a two-brick course bears on BOTH of them, "
					 "so the Auto preview must say 2 joints; it says %d"),
				Preview.JointCount),
			Preview.JointCount, 2);

		TestTrue(
			*FString::Printf(
				TEXT("with no override the plate's bearings are the inferred DryStone — timber is not "
					 "compression-dominant, so the inference hands out the weakest bearing it has. "
					 "The preview named %s"),
				OverrideProfileName(Preview.JointProfile)),
			Preview.JointProfile == &DryStone);

		Subsystem.PlaceBuildPiece(
			StructureId, OverridePlateCentreCm, OverrideHalfPlateCm, Timber,
			EPlacementMode::Snap, AutoOverride);

		FStructureBinding* const Binding = Subsystem.Find(StructureId);

		// One head joint from the seeded course, then the plate's two bearings.
		if (Binding == nullptr || Binding->GetStructure().NumConnections() != 3)
		{
			AddError(FString::Printf(
				TEXT("fixture: the seeded course plus the Auto plate must give 3 joints (one head, "
					 "two bearings); it gave %d"),
				Binding != nullptr ? Binding->GetStructure().NumConnections() : INDEX_NONE));

			TestWorld.End();
			return true;
		}

		const FVector PlateCentreCm = Binding->GetBinding(2).Box.CentreCm;

		TestTrue(
			FString::Printf(
				TEXT("fixture: the plate must bear one joint above the course, at (0, 0, 12.5); it is "
					 "at (%g, %g, %g)"),
				PlateCentreCm.X, PlateCentreCm.Y, PlateCentreCm.Z),
			PlateCentreCm.Equals(OverridePlateCentreCm, KINDA_SMALL_NUMBER));

		for (int32 Index = 1; Index < 3; ++Index)
		{
			const FConnection& Bearing = Binding->GetStructure().GetConnection(Index);

			AutoBearingShapes.Add(ShapeOf(Bearing));

			TestEqual(
				FString::Printf(
					TEXT("fixture: bearing %d is a horizontal face, so |normal.Z| should be 1, got %g"),
					Index, FMath::Abs(Bearing.InterfaceNormal.Z)),
				FMath::Abs(Bearing.InterfaceNormal.Z), 1.0, 1.0e-9);

			TestEqual(
				FString::Printf(
					TEXT("fixture: bearing %d covers the whole brick, 21.5 x 10.25 = 220.375 cm2, got "
						 "%g"),
					Index, Bearing.InterfaceAreaSqCm),
				Bearing.InterfaceAreaSqCm, OverrideBearingAreaSqCm, 1.0e-6);

			CheckOverrideProfile(
				*this,
				FString::Printf(TEXT("AUTO baseline: bearing %d is the inferred DryStone: "), Index),
				Bearing.Strength,
				DryStone);
		}
	}

	// Four: the same plate screwed; both bearings must be screws.

	{
		const int32 StructureId = SeedTwoBrickCourse(Subsystem);

		const FBuildPreview Preview = Subsystem.PreviewBuildPiece(
			StructureId, OverridePlateCentreCm, OverrideHalfPlateCm, Timber,
			EPlacementMode::Snap, ScrewOverride);

		TestEqual(
			FString::Printf(
				TEXT("the screwed plate must form the SAME two bearings, not fewer; the preview says "
					 "%d"),
				Preview.JointCount),
			Preview.JointCount, 2);

		TestTrue(
			*FString::Printf(
				TEXT("and the preview must name Screw by address, it named %s"),
				OverrideProfileName(Preview.JointProfile)),
			Preview.JointProfile == ScrewOverride);

		Subsystem.PlaceBuildPiece(
			StructureId, OverridePlateCentreCm, OverrideHalfPlateCm, Timber,
			EPlacementMode::Snap, ScrewOverride);

		FStructureBinding* const Binding = Subsystem.Find(StructureId);

		if (Binding == nullptr || Binding->GetStructure().NumConnections() != 3)
		{
			AddError(FString::Printf(
				TEXT("the screwed plate must leave the structure with 3 joints exactly as the Auto "
					 "one did; it has %d"),
				Binding != nullptr ? Binding->GetStructure().NumConnections() : INDEX_NONE));

			TestWorld.End();
			return true;
		}

		// Existing joints are not re-priced: the override applies only to the new placement.
		CheckOverrideProfile(
			*this,
			TEXT("the course's existing head joint is NOT re-priced by a later override: "),
			Binding->GetStructure().GetConnection(0).Strength,
			GeneralPurposeMortarPerpend);

		for (int32 Index = 1; Index < 3; ++Index)
		{
			const FConnection& Bearing = Binding->GetStructure().GetConnection(Index);

			// The second bearing is the real test: a Joints[0]-only override would leave it dry.
			CheckOverrideProfile(
				*this,
				FString::Printf(
					TEXT("SCREW: bearing %d of 2 carries the override: "), Index),
				Bearing.Strength,
				Screw);

			const bool bGeometryMatches = AutoBearingShapes.IsValidIndex(Index - 1)
				&& ShapesAreTheSameJoint(ShapeOf(Bearing), AutoBearingShapes[Index - 1]);

			TestTrue(
				*FString::Printf(
					TEXT("SCREW: bearing %d must be the SAME joint the Auto run formed — Auto gave "
						 "%s, this gave %s"),
					Index,
					AutoBearingShapes.IsValidIndex(Index - 1)
						? *DescribeShape(AutoBearingShapes[Index - 1])
						: TEXT("<no Auto bearing>"),
					*DescribeShape(ShapeOf(Bearing))),
				bGeometryMatches);
		}
	}

	/*
	 * Five: a jointless placement names no profile, override or not. A ghost card reading "Screw"
	 * over a piece held by nothing would tell the player the opposite of the truth. The Auto row is
	 * the control.
	 */
	{
		struct FLonelyCase
		{
			const TCHAR* Description;
			const FConnectionStrength* Override;
		};

		const FLonelyCase LonelyCases[] = {
			{
				TEXT("SCREW over empty ground: the player has chosen a fastener and there is nothing "
					 "within reach to fasten to"),
				ScrewOverride,
			},
			{
				TEXT("AUTO over empty ground — the control, which already reads null, so a reading "
					 "nulled unconditionally cannot pass for the fix"),
				AutoOverride,
			},
		};

		for (const FLonelyCase& Case : LonelyCases)
		{
			const int32 StructureId = SeedOneBrick(Subsystem);

			const FBuildPreview Preview = Subsystem.PreviewBuildPiece(
				StructureId, OverrideLonelyCentreCm, OverrideHalfBrickCm, ClayBrick,
				EPlacementMode::Free, Case.Override);

			TestTrue(
				*FString::Printf(
					TEXT("fixture: %s — Free honours the cursor verbatim, so the preview must be valid"),
					Case.Description),
				Preview.bValid);

			TestEqual(
				FString::Printf(
					TEXT("fixture: %s — the piece must form NO joint, or there is nothing to claim; the "
						 "preview says %d"),
					Case.Description, Preview.JointCount),
				Preview.JointCount, 0);

			TestTrue(
				*FString::Printf(
					TEXT("%s: A JOINTLESS PLACEMENT MUST NAME NO PROFILE. The ghost card's job is to say "
						 "what this click builds, and what it builds here is a piece held by NOTHING — a "
						 "card reading '%s' tells the player the opposite, over the one placement that "
						 "is one Run away from lying on the ground. It named %s"),
					Case.Description, OverrideProfileName(Case.Override),
					OverrideProfileName(Preview.JointProfile)),
				Preview.JointProfile == nullptr);

			// The commit still lays the piece, with no joint (Free must not refuse the pose).
			const FPieceRef Ref = Subsystem.PlaceBuildPiece(
				StructureId, OverrideLonelyCentreCm, OverrideHalfBrickCm, ClayBrick,
				EPlacementMode::Free, Case.Override);

			FStructureBinding* const Binding = Subsystem.Find(StructureId);

			TestTrue(
				*FString::Printf(
					TEXT("%s: the commit must still LAY the piece — Free is the deliberate escape — and "
						 "form no joint; it landed as piece %d with %d joint(s)"),
					Case.Description, Ref.PieceIndex,
					Binding != nullptr ? Binding->GetStructure().NumConnections() : INDEX_NONE),
				Binding != nullptr && Ref.PieceIndex == 1
					&& Binding->GetStructure().NumConnections() == 0);
		}
	}

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
