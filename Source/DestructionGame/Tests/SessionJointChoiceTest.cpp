// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/PieceMenu.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/SessionToolbar.h"
#include "Core/Structure.h"
#include "Core/StructureBinding.h"
#include "DestructionGamePlayerController.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/BrickActor.h"
#include "World/BuildModeComponent.h"
#include "World/DestructionStructureSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Uniquely named namespace: unity builds merge files, so anonymous namespaces can collide. */
namespace SessionJointChoiceTestSupport
{
	/*
	 * Course heights, transcribed rather than taken from DestructionSession::CoursePlaneZCm so a
	 * wrong plane function can't agree with itself (DESIGN §8). Course 0 brick centre = half its
	 * 6.5 cm height. Course 1 plate (10 cm thick) = 7.5 + 5.0 = 12.5, one 1 cm joint above the
	 * brick top, so it bears rather than intersects.
	 */
	constexpr double JointChoiceBrickCourse0ZCm = 3.25;
	constexpr double JointChoicePlateCourse1ZCm = 12.5;

	/** The seed brick and its neighbour one 22.5 cm pitch along. */
	const FVector JointChoiceSeedCentreCm(0.0, 0.0, JointChoiceBrickCourse0ZCm);
	const FVector JointChoiceSecondCentreCm(22.5, 0.0, JointChoiceBrickCourse0ZCm);

	/** The plate: centred on the seed brick, bearing on both. */
	const FVector JointChoicePlateCentreCm(0.0, 0.0, JointChoicePlateCourse1ZCm);

	/** Cursor X for the second brick, 0.5 cm off the bond, so landing at 22.5 proves the snap. */
	constexpr double JointChoiceSecondCursorXCm = 22.0;

	/** Build rays start well above the plane and aim at the floor. */
	constexpr double JointChoiceRayStartZCm = 300.0;
	constexpr double JointChoiceRayEndZCm = 0.0;

	/** Inspect-ray half-length along Y; well beyond the 10.25 cm brick depth. */
	constexpr double JointChoiceInspectReachCm = 100.0;

	FVector JointChoiceRayStart(double XCm)
	{
		return FVector(XCm, 0.0, JointChoiceRayStartZCm);
	}

	FVector JointChoiceRayEnd(double XCm)
	{
		return FVector(XCm, 0.0, JointChoiceRayEndZCm);
	}

	/** Compare all five fields: FConnectionStrength has no operator==, and siblings differ by one field. */
	void CheckSessionProfile(
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

	/** Case-insensitive substring check, so wording can change without breaking tests. */
	bool SaysWord(const FString& Text, const TCHAR* Word)
	{
		return Text.Contains(FString(Word), ESearchCase::IgnoreCase);
	}

	/** Joint rows on one line, for failure messages. */
	FString DescribeJointRows(TArrayView<const FInspectorJointRow> Rows)
	{
		if (Rows.Num() == 0)
		{
			return TEXT("<no joint rows>");
		}

		FString Line;

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s'%s'"), Index == 0 ? TEXT("") : TEXT(" | "), *Rows[Index].Text);
		}

		return Line;
	}

	FPieceRef MakeSessionRef(int32 StructureId, int32 PieceIndex)
	{
		FPieceRef Ref;
		Ref.StructureId = StructureId;
		Ref.PieceIndex = PieceIndex;
		return Ref;
	}

	// Presenter fixture geometry.

	/** Arbitrary structure id for the presenter fixture. */
	constexpr int32 JointChoicePresenterStructure = 7;

	/** Brick half-extents and joint thickness, cm. */
	const FVector JointChoicePresenterHalfBrickCm(10.75, 5.125, 3.25);
	constexpr double JointChoicePresenterJointCm = 1.0;

	/**
	 * One neighbour per face, so the subject carries six different profiles at once. The two head
	 * joints (+X, -X) share a role but not a profile, which catches a presenter naming the role.
	 */
	const FVector JointChoicePresenterOffsetsCm[6] = {
		FVector(22.5, 0.0, 0.0),
		FVector(-22.5, 0.0, 0.0),
		FVector(0.0, 11.25, 0.0),
		FVector(0.0, -11.25, 0.0),
		FVector(0.0, 0.0, 7.5),
		FVector(0.0, 0.0, -7.5),
	};
}

/**
 * UI-6, session half: OnToolbarButton(JointScrew) reaches both the session state and the
 * UBuildModeComponent, the next Build-mode placement puts that profile on every joint of the new
 * piece, and inspecting it in Destroy mode names the profile in the joint rows.
 *
 * World.BuildMode.JointOverrideRidesThroughPlacement covers the subsystem door; this covers the
 * wire from chip to component to door, which the model tests in Core.SessionToolbar.* cannot see.
 * The inspector is part of the claim because a screwed and a dry-bedded plate look identical;
 * the joint row naming the profile is the only way a player can tell.
 *
 * Joints store a copy of the strength, so the name comes back via FindConnectionProfileRow's
 * five-field match (Core/Profiles/ConnectionProfiles.h), and the claim is on the row text.
 *
 * Needs a world (spawned bricks, a real line trace) but no ticking. All assertions are mechanism
 * readings; nothing moves.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionJointChoiceRidesThroughTheSessionTest,
	"DestructionGame.World.Session.JointChoiceRidesThroughTheSession",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionJointChoiceRidesThroughTheSessionTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionProfiles;
	using namespace DestructionSession;
	using namespace SessionJointChoiceTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		TestWorld.End();
		return true;
	}

	ADestructionGamePlayerController* const Controller =
		SpawnControllerWithLocalPlayer(*this, TestWorld.World);

	if (Controller == nullptr)
	{
		TestWorld.End();
		return true;
	}

	UBuildModeComponent* const Build = Controller->GetBuildComponent();

	TestNotNull(
		TEXT("fixture: a spawned controller must already carry its UBuildModeComponent"), Build);

	if (Build == nullptr)
	{
		TestWorld.End();
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	if (!Controller->OnToolbarButton(EToolbarButtonId::ModeBuild))
	{
		AddError(TEXT("fixture: the Build tab must be clickable for any of this to run"));
		TestWorld.End();
		return true;
	}

	const int32 StructureId = Build->GetStructureId();

	// 1. The session opens on Auto, and the component agrees.

	TestTrue(
		*FString::Printf(
			TEXT("a session opens with the joint choice on Auto — the inference decides until the "
				 "player says otherwise; the state reads %d"),
			static_cast<int32>(Controller->GetSessionToolbarState().Joint)),
		Controller->GetSessionToolbarState().Joint == EJointChoice::Auto);

	TestTrue(
		*FString::Printf(
			TEXT("and so does the component, which is the copy that actually reaches the door; it "
				 "reads %d"),
			static_cast<int32>(Build->JointChoice)),
		Build->JointChoice == EJointChoice::Auto);

	// 2. Two bricks laid on Auto are bonded by the inferred perpend.

	{
		TestTrue(
			TEXT("fixture: the first click on the build plane must lay a brick"),
			Controller->PrimaryAlongRay(JointChoiceRayStart(0.0), JointChoiceRayEnd(0.0)));

		TestTrue(
			TEXT("fixture: the second click, 0.5 cm off the bond, must lay another"),
			Controller->PrimaryAlongRay(
				JointChoiceRayStart(JointChoiceSecondCursorXCm),
				JointChoiceRayEnd(JointChoiceSecondCursorXCm)));

		FStructureBinding* const Binding = Subsystem.Find(StructureId);

		if (Binding == nullptr || Binding->NumPieces() != 2
			|| Binding->GetStructure().NumConnections() != 1)
		{
			AddError(FString::Printf(
				TEXT("fixture: two clicks must give two bricks and one head joint; the build holds %d "
					 "pieces and %d joints"),
				Binding != nullptr ? Binding->NumPieces() : INDEX_NONE,
				Binding != nullptr ? Binding->GetStructure().NumConnections() : INDEX_NONE));

			TestWorld.End();
			return true;
		}

		TestTrue(
			TEXT("fixture: the second brick must be pulled onto the bond at (22.5, 0, 3.25)"),
			Binding->GetBinding(1).Box.CentreCm.Equals(
				JointChoiceSecondCentreCm, KINDA_SMALL_NUMBER));

		CheckSessionProfile(
			*this,
			TEXT("on Auto the course's head joint is the INFERRED perpend: "),
			Binding->GetStructure().GetConnection(0).Strength,
			GeneralPurposeMortarPerpend);
	}

	// 3. Picking Screw reaches the component.

	TestTrue(
		TEXT("the Screw chip must be clickable — a joint choice has no precondition"),
		Controller->OnToolbarButton(EToolbarButtonId::JointScrew));

	TestTrue(
		*FString::Printf(
			TEXT("the click must move the SESSION's own record of the choice; it reads %d"),
			static_cast<int32>(Controller->GetSessionToolbarState().Joint)),
		Controller->GetSessionToolbarState().Joint == EJointChoice::Screw);

	// The state drives the strip; the component drives the next placement. Both must change.
	TestTrue(
		*FString::Printf(
			TEXT("and it must PUSH that choice onto the build component; the component reads %d"),
			static_cast<int32>(Build->JointChoice)),
		Build->JointChoice == EJointChoice::Screw);

	// 4. A plate laid on that course is screwed to both bricks.

	FPieceRef PlateRef;

	{
		TestTrue(
			TEXT("fixture: the timber plate chip must be clickable"),
			Controller->OnToolbarButton(EToolbarButtonId::PieceTimberPlate));

		TestTrue(
			TEXT("fixture: Course up must be clickable, to put the plate's plane on top of the course"),
			Controller->OnToolbarButton(EToolbarButtonId::CourseUp));

		TestTrue(
			TEXT("fixture: the click on the bearing plane must lay the plate"),
			Controller->PrimaryAlongRay(JointChoiceRayStart(0.0), JointChoiceRayEnd(0.0)));

		FStructureBinding* const Binding = Subsystem.Find(StructureId);

		if (Binding == nullptr || Binding->NumPieces() != 3)
		{
			AddError(FString::Printf(
				TEXT("fixture: the plate must land as the third piece; the build holds %d"),
				Binding != nullptr ? Binding->NumPieces() : INDEX_NONE));

			TestWorld.End();
			return true;
		}

		PlateRef = MakeSessionRef(StructureId, 2);

		const FVector PlateCentreCm = Binding->GetBinding(2).Box.CentreCm;

		TestTrue(
			FString::Printf(
				TEXT("fixture: the plate must bear one joint above the course, at (0, 0, 12.5); it is "
					 "at (%g, %g, %g)"),
				PlateCentreCm.X, PlateCentreCm.Y, PlateCentreCm.Z),
			PlateCentreCm.Equals(JointChoicePlateCentreCm, KINDA_SMALL_NUMBER));

		// Three joints: the head joint plus one bearing per brick. Both bearings must get the override.
		TestEqual(
			FString::Printf(
				TEXT("the plate spans both bricks, so the build must hold 3 joints — one head and "
					 "two bearings; it holds %d"),
				Binding->GetStructure().NumConnections()),
			Binding->GetStructure().NumConnections(), 3);

		if (Binding->GetStructure().NumConnections() == 3)
		{
			for (int32 Index = 1; Index < 3; ++Index)
			{
				CheckSessionProfile(
					*this,
					FString::Printf(
						TEXT("the screwed plate's bearing %d of 2 carries Screw: "), Index),
					Binding->GetStructure().GetConnection(Index).Strength,
					Screw);
			}

			// The choice applies only to new placements; existing joints keep their profile.
			CheckSessionProfile(
				*this,
				TEXT("the course's existing head joint is NOT re-priced by the later choice: "),
				Binding->GetStructure().GetConnection(0).Strength,
				GeneralPurposeMortarPerpend);
		}
	}

	// 5. In Destroy mode, the joint rows name the profile.

	{
		TestTrue(
			TEXT("the Destroy tab is always live"),
			Controller->OnToolbarButton(EToolbarButtonId::ModeDestroy));

		const FVector InspectStart(
			JointChoicePlateCentreCm.X,
			JointChoicePlateCentreCm.Y - JointChoiceInspectReachCm,
			JointChoicePlateCentreCm.Z);

		const FVector InspectEnd(
			JointChoicePlateCentreCm.X,
			JointChoicePlateCentreCm.Y + JointChoiceInspectReachCm,
			JointChoicePlateCentreCm.Z);

		Controller->PrimaryAlongRay(InspectStart, InspectEnd);

		TestTrue(
			TEXT("fixture: clicking the plate in Destroy mode must put its menu up"),
			Controller->IsPieceMenuShown());

		Controller->SetInspectedPiece(PlateRef);

		const FPieceMenuInspector Inspector = Controller->PieceMenuInspectorForSelection();

		TestEqual(
			FString::Printf(
				TEXT("fixture: the inspected plate must break out its two bearings; it broke out %d "
					 "— [%s]"),
				Inspector.Joints.Num(), *DescribeJointRows(Inspector.Joints)),
			Inspector.Joints.Num(), 2);

		for (int32 Index = 0; Index < Inspector.Joints.Num(); ++Index)
		{
			// The row must contain "screw"; the rest of the wording is the presenter's.
			TestTrue(
				*FString::Printf(
					TEXT("the screwed plate's joint row %d must NAME the profile that fastens it — it "
						 "reads '%s'"),
					Index, *Inspector.Joints[Index].Text),
				SaysWord(Inspector.Joints[Index].Text, TEXT("screw")));
		}

		/*
		 * Discrimination: brick 0 has a mortared head joint and a screwed bearing, so exactly one
		 * row each must say mortar and screw (catches a presenter printing a constant). It is
		 * clicked into the selection first, because BuildPieceMenuInspector ignores a ref outside it.
		 */
		const FVector BrickInspectStart(
			JointChoiceSeedCentreCm.X,
			JointChoiceSeedCentreCm.Y - JointChoiceInspectReachCm,
			JointChoiceSeedCentreCm.Z);

		const FVector BrickInspectEnd(
			JointChoiceSeedCentreCm.X,
			JointChoiceSeedCentreCm.Y + JointChoiceInspectReachCm,
			JointChoiceSeedCentreCm.Z);

		Controller->PrimaryAlongRay(BrickInspectStart, BrickInspectEnd);

		Controller->SetInspectedPiece(MakeSessionRef(StructureId, 0));

		const FPieceMenuInspector BrickInspector = Controller->PieceMenuInspectorForSelection();

		TestEqual(
			FString::Printf(
				TEXT("fixture: brick 0 wears two joints — the head into its neighbour and the plate's "
					 "bearing over it; %d were broken out — [%s]"),
				BrickInspector.Joints.Num(), *DescribeJointRows(BrickInspector.Joints)),
			BrickInspector.Joints.Num(), 2);

		int32 MortaredRows = 0;
		int32 ScrewedRows = 0;

		for (const FInspectorJointRow& Row : BrickInspector.Joints)
		{
			MortaredRows += SaysWord(Row.Text, TEXT("mortar")) || SaysWord(Row.Text, TEXT("perpend"))
				? 1 : 0;

			ScrewedRows += SaysWord(Row.Text, TEXT("screw")) ? 1 : 0;
		}

		TestEqual(
			FString::Printf(
				TEXT("brick 0 carries one mortared head joint and one screwed bearing, so exactly one "
					 "of its rows may say screw; %d did — [%s]"),
				ScrewedRows, *DescribeJointRows(BrickInspector.Joints)),
			ScrewedRows, 1);

		TestEqual(
			FString::Printf(
				TEXT("and exactly one must name the mortar that bonds it to its neighbour; %d did — "
					 "[%s]"),
				MortaredRows, *DescribeJointRows(BrickInspector.Joints)),
			MortaredRows, 1);
	}

	TestWorld.End();

	return true;
}

/**
 * Each joint row names its profile, and same-role joints with different profiles read
 * differently. World-free: BuildPieceMenuInspector takes a binding and returns strings.
 *
 * One subject with six neighbours, one per face, each joint a different library row
 * (GeneralPurposeMortar, GeneralPurposeMortarPerpend, DryStone, Nail, Screw, Bolt). This catches a
 * constant string, naming by role (the two head joints differ), and confusing bed mortar with its
 * weaker perpend. A second fixture checks an unshipped strength reads "custom".
 *
 * Rows must contain the profile's word, not match a full sentence. The perpend alone may also say
 * "mortar". Needs no world and no solve.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FJointRowNamesTheProfileTest,
	"DestructionGame.Presenter.JointRowNamesTheProfile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FJointRowNamesTheProfileTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace SessionJointChoiceTestSupport;

	struct FProfileWordCase
	{
		const TCHAR* Description;
		const FConnectionStrength* Profile;

		/** The word this row MUST contain. */
		const TCHAR* Word;

		/** Another row's word this row may also contain, or null. Only the perpend has one ("mortar"). */
		const TCHAR* AlsoAllowed;
	};

	const FProfileWordCase Cases[] = {
		{
			TEXT("the full bed bond — the strongest thing the library ships for masonry"),
			&GeneralPurposeMortar, TEXT("mortar"), nullptr,
		},
		{
			TEXT("the WEAK perpend: the same mortar in a vertical joint, both bond axes knocked down"),
			&GeneralPurposeMortarPerpend, TEXT("perpend"), TEXT("mortar"),
		},
		{
			TEXT("dry stone — friction and compression, no bond at all"),
			&DryStone, TEXT("dry"), nullptr,
		},
		{
			TEXT("a nail"),
			&Nail, TEXT("nail"), nullptr,
		},
		{
			TEXT("a screw"),
			&Screw, TEXT("screw"), nullptr,
		},
		{
			TEXT("a bolt"),
			&Bolt, TEXT("bolt"), nullptr,
		},
	};

	// Handle 0 is the subject; handles 1..6 are neighbours in offset order, so case N is connection N.
	FStructureBinding Binding;
	Binding.StructureId = JointChoicePresenterStructure;

	const FPieceBox SubjectBox{ FVector::ZeroVector, JointChoicePresenterHalfBrickCm };

	const int32 Subject = Binding.AddPiece(
		2.0, /*bIsGrounded*/ false, nullptr, SubjectBox, &ClayBrick);

	TestEqual(TEXT("fixture: the subject must be handle 0"), Subject, 0);

	for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(Cases)); ++Index)
	{
		const FPieceBox NeighbourBox{
			JointChoicePresenterOffsetsCm[Index], JointChoicePresenterHalfBrickCm };

		// Ground the bottom neighbour so the fixture is not stranded (nothing is solved here anyway).
		const bool bGrounded = Index == 5;

		const int32 Neighbour = Binding.AddPiece(
			1.0, bGrounded, nullptr, NeighbourBox, &ClayBrick);

		FConnection Conn;

		const bool bMade = MakeInterface(
			Subject, SubjectBox, Neighbour, NeighbourBox,
			JointChoicePresenterJointCm, *Cases[Index].Profile, Conn);

		TestTrue(
			*FString::Printf(
				TEXT("fixture: %s — the neighbour at (%g, %g, %g) must abut the subject across a real "
					 "face"),
				Cases[Index].Description,
				JointChoicePresenterOffsetsCm[Index].X,
				JointChoicePresenterOffsetsCm[Index].Y,
				JointChoicePresenterOffsetsCm[Index].Z),
			bMade);

		if (bMade)
		{
			TestEqual(
				*FString::Printf(
					TEXT("fixture: %s — its joint must be connection %d"),
					Cases[Index].Description, Index),
				Binding.AddConnection(Conn), Index);
		}
	}

	const FPieceRef SubjectRef = MakeSessionRef(JointChoicePresenterStructure, Subject);
	const TArray<FPieceRef> Selected = { SubjectRef };

	const FPieceMenuInspector Inspector =
		BuildPieceMenuInspector(Binding, Selected, SubjectRef);

	const int32 CaseCount = static_cast<int32>(UE_ARRAY_COUNT(Cases));

	TestEqual(
		FString::Printf(
			TEXT("fixture: the subject wears six joints, so six rows must be broken out; %d were — "
				 "[%s]"),
			Inspector.Joints.Num(), *DescribeJointRows(Inspector.Joints)),
		Inspector.Joints.Num(), CaseCount);

	if (Inspector.Joints.Num() != CaseCount)
	{
		return true;
	}

	for (const FInspectorJointRow& Row : Inspector.Joints)
	{
		if (Row.ConnectionIndex < 0 || Row.ConnectionIndex >= CaseCount)
		{
			AddError(FString::Printf(
				TEXT("a joint row names connection %d, which this fixture never built — '%s'"),
				Row.ConnectionIndex, *Row.Text));

			continue;
		}

		const FProfileWordCase& Case = Cases[Row.ConnectionIndex];

		TestTrue(
			*FString::Printf(
				TEXT("%s: the row for connection %d must contain '%s' — a row that names only its "
					 "role reads identically for two joints that behave nothing alike. It reads '%s'"),
				Case.Description, Row.ConnectionIndex, Case.Word, *Row.Text),
			SaysWord(Row.Text, Case.Word));

		// It names only its own profile, not any other row's word.
		for (int32 OtherIndex = 0; OtherIndex < CaseCount; ++OtherIndex)
		{
			if (OtherIndex == Row.ConnectionIndex)
			{
				continue;
			}

			const FProfileWordCase& Other = Cases[OtherIndex];

			const bool bExempt = Case.AlsoAllowed != nullptr
				&& FString(Case.AlsoAllowed).Equals(Other.Word, ESearchCase::IgnoreCase);

			if (bExempt)
			{
				continue;
			}

			TestFalse(
				*FString::Printf(
					TEXT("%s: the row for connection %d must NOT also say '%s' — it reads '%s'"),
					Case.Description, Row.ConnectionIndex, Other.Word, *Row.Text),
				SaysWord(Row.Text, Other.Word));
		}
	}

	/*
	 * An unshipped strength reads "custom", not the nearest shipped name. Its own fixture because
	 * the six faces are used. Unreachable via the UI today, but one authored joint or save file away.
	 * 0.01 MPa off Screw's tensile, so a lookup with any tolerance, or one comparing only four
	 * fields, would wrongly say Screw.
	 */
	{
		FConnectionStrength Unshipped = Screw;
		Unshipped.TensileStrengthMPa += 0.01;

		FStructureBinding Odd;
		Odd.StructureId = JointChoicePresenterStructure;

		const FPieceBox OddSubjectBox{ FVector::ZeroVector, JointChoicePresenterHalfBrickCm };

		const FPieceBox OddNeighbourBox{
			JointChoicePresenterOffsetsCm[0], JointChoicePresenterHalfBrickCm };

		const int32 OddSubject = Odd.AddPiece(2.0, /*bIsGrounded*/ true, nullptr, OddSubjectBox, &ClayBrick);
		const int32 OddNeighbour = Odd.AddPiece(1.0, /*bIsGrounded*/ true, nullptr, OddNeighbourBox, &ClayBrick);

		FConnection OddConn;

		const bool bOddMade = MakeInterface(
			OddSubject, OddSubjectBox, OddNeighbour, OddNeighbourBox,
			JointChoicePresenterJointCm, Unshipped, OddConn);

		TestTrue(TEXT("fixture: the unshipped-profile neighbour must abut across a real face"), bOddMade);

		if (bOddMade)
		{
			Odd.AddConnection(OddConn);

			const FPieceRef OddRef = MakeSessionRef(JointChoicePresenterStructure, OddSubject);

			const TArray<FPieceRef> OddSelected = { OddRef };

			const FPieceMenuInspector OddInspector =
				BuildPieceMenuInspector(Odd, OddSelected, OddRef);

			TestEqual(
				FString::Printf(
					TEXT("fixture: the odd subject wears one joint, so one row must be broken out; %d "
						 "were — [%s]"),
					OddInspector.Joints.Num(), *DescribeJointRows(OddInspector.Joints)),
				OddInspector.Joints.Num(), 1);

			if (OddInspector.Joints.Num() == 1)
			{
				const FInspectorJointRow& OddRow = OddInspector.Joints[0];

				TestTrue(
					*FString::Printf(
						TEXT("A JOINT THIS LIBRARY NEVER SHIPPED MUST READ 'custom'. Going quiet would "
							 "print the role-only sentence — 'head', which is the same line for a "
							 "screwed joint and a dry one — with nothing to say a reading was missing. "
							 "It reads '%s'"),
						*OddRow.Text),
					SaysWord(OddRow.Text, TEXT("custom")));

				for (const FProfileWordCase& Case : Cases)
				{
					TestFalse(
						*FString::Printf(
							TEXT("and it must name NONE of the shipped rows — it is 0.01 MPa off Screw's "
								 "withdrawal and it is not a screw; naming the nearest row is a "
								 "plausible lie. It must not say '%s', and it reads '%s'"),
							Case.Word, *OddRow.Text),
						SaysWord(OddRow.Text, Case.Word));
				}
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
