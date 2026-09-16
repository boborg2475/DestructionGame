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

/**
 * NAMED NAMESPACE, not anonymous, and named differently from every other one in this directory.
 * An anonymous namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build
 * merges many files into one — at which point two file-local names that collide are a hard compile
 * error between files that never refer to each other. See CURRENT_STATE.md; the `using namespace`
 * lives inside each RunTest for the same reason.
 */
namespace SessionJointChoiceTestSupport
{
	/*
	 * THE GRID AND THE RESTS-ON-THE-GROUND CONVENTION, TRANSCRIBED RATHER THAN IMPORTED (DESIGN §8,
	 * 2026-09-15). A 21.5 x 10.25 x 6.5 cm brick on a 1 cm joint gives the 22.5 x 11.25 x 7.5
	 * coordinating grid; course 0 centres a brick at its own half height, 3.25, so its underside is
	 * ON the earth. A plate is 10 cm thick, so course 1 centres it at 7.5 + 5.0 = 12.5 — which is
	 * exactly one joint above the top of a course-0 brick (6.5 + 1.0 + 5.0), and that is what makes
	 * the plate BEAR rather than intersect.
	 *
	 * Calling DestructionSession::CoursePlaneZCm here would make this test agree with the plane
	 * function however wrong it is, which is one of the things it exists to catch.
	 */
	constexpr double JointChoiceBrickCourse0ZCm = 3.25;
	constexpr double JointChoicePlateCourse1ZCm = 12.5;

	/** The seed brick and its same-course neighbour, one 22.5 cm pitch along. */
	const FVector JointChoiceSeedCentreCm(0.0, 0.0, JointChoiceBrickCourse0ZCm);
	const FVector JointChoiceSecondCentreCm(22.5, 0.0, JointChoiceBrickCourse0ZCm);

	/** Where the plate lands: centred on the seed brick, bearing across both. */
	const FVector JointChoicePlateCentreCm(0.0, 0.0, JointChoicePlateCourse1ZCm);

	/**
	 * The cursor X for the second brick — 0.5 cm off the bond, so the assertion that it lands at
	 * 22.5 reads the SOLVER rather than the request.
	 */
	constexpr double JointChoiceSecondCursorXCm = 22.0;

	/** A pointing ray is a DIRECTION: it starts well above the build plane and aims at the floor. */
	constexpr double JointChoiceRayStartZCm = 300.0;
	constexpr double JointChoiceRayEndZCm = 0.0;

	/** A brick is 10.25 cm deep on Y, so +/- 100 cm crosses it entirely with nothing else in the way. */
	constexpr double JointChoiceInspectReachCm = 100.0;

	FVector JointChoiceRayStart(double XCm)
	{
		return FVector(XCm, 0.0, JointChoiceRayStartZCm);
	}

	FVector JointChoiceRayEnd(double XCm)
	{
		return FVector(XCm, 0.0, JointChoiceRayEndZCm);
	}

	/** Full-field profile identity — FConnectionStrength has no operator==, and siblings differ by one axis. */
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

	/** Case-insensitive "does this sentence contain that word", so the wording may be retuned. */
	bool SaysWord(const FString& Text, const TCHAR* Word)
	{
		return Text.Contains(FString(Word), ESearchCase::IgnoreCase);
	}

	/** Every joint row on one line, so a failure reads without a debugger. */
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

	/* --- THE PRESENTER FIXTURE'S OWN GEOMETRY ------------------------------------------------ */

	/** The structure the presenter fixture identifies itself as. Any id will do; it has to be one. */
	constexpr int32 JointChoicePresenterStructure = 7;

	/** One brick, halved, and the six neighbour offsets that abut it across a 1 cm joint. */
	const FVector JointChoicePresenterHalfBrickCm(10.75, 5.125, 3.25);
	constexpr double JointChoicePresenterJointCm = 1.0;

	/**
	 * SIX NEIGHBOURS, ONE PER FACE, SO ONE BRICK CAN WEAR SIX DIFFERENT PROFILES AT ONCE.
	 *
	 * A box has exactly six faces and the library has exactly six profiles a player can choose, so
	 * the fixture is one subject surrounded on every side. That is what makes the claim a sweep over
	 * the whole vocabulary rather than one row about screws: a presenter that printed a constant, or
	 * that named the profile off the joint's ROLE instead of its strengths, fails on the pairs that
	 * share a role — the two head joints on +X and -X carry different profiles and must read
	 * differently.
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
 * UI-6, THE SESSION HALF — THE JOINT CHIP THE PLAYER CLICKS IS WHAT FASTENS THE PIECE THEY THEN
 * LAY, AND THE DETAILS WINDOW SAYS SO AFTERWARDS.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * `OnToolbarButton(JointScrew)` puts the choice on the controller's session state AND on its
 * `UBuildModeComponent`; the next `PrimaryAlongRay` in Build mode lays a piece every one of whose
 * joints carries that profile; and inspecting that piece in Destroy mode reads the profile's own
 * name back out of the joint rows.
 *
 * =====================================================================================
 * WHY THIS IS A SEPARATE TEST FROM THE SUBSYSTEM'S
 * =====================================================================================
 *
 * `World.BuildMode.JointOverrideRidesThroughPlacement` pins that a `const FConnectionStrength*`
 * handed to the door replaces every joint's profile. What it cannot see is the WIRE: that the chip
 * reaches the component, that the component hands it to the door, and that nothing in between
 * drops it. CURRENT_STATE records the component's kind, course and placement as a parallel copy of
 * three state fields precisely because that wire is where a session falls apart — a chip that lit
 * correctly and laid mortar anyway would pass every model sweep in `Core.SessionToolbar.*`.
 *
 * =====================================================================================
 * AND WHY THE INSPECTOR IS PART OF THE SAME CLAIM
 * =====================================================================================
 *
 * The choice changes COMMITTED PHYSICS and nothing on screen moves when it does: a screwed plate
 * and a dry-bedded one sit in exactly the same place and look identical until the structure is
 * run. So the only way a player can tell what they built is the details window, and a joint row
 * that names only its ROLE — "bed below", "head" — is the same sentence for both. Naming the
 * profile is what makes the setting observable at all, which is the same argument the support
 * words and the margin bands were built under.
 *
 * THE IDENTITY TRAP THIS SLICE CLOSED, AND WHY THE CLAIM IS STILL ON THE TEXT.
 * `FNamedConnectionProfile::Strength` (Core/Profiles/ConnectionProfiles.h) is now a REFERENCE to the
 * shipped extern — it was held BY VALUE, exactly as `FNamedMaterialProfile::Profile` was until S7,
 * and a name lookup written the obvious way (comparing a joint's strength against
 * `AllConnectionProfiles()` BY ADDRESS) answered "no such row" for every joint in the game. A joint
 * in a built wall has no address left to compare in any case — `FStructure::AddConnection` stores a
 * COPY — so the route back to the name is `FindConnectionProfileRow`'s five-field match, and the
 * reference is what makes ITS answer the library's own row. Which is why the claim here is on the
 * ROW TEXT a player reads rather than on a field somebody could fill in from the override alone.
 *
 * NEEDS A TICKING WORLD: a real world — the controller spawns bricks, the ghost is an actor and the
 * Destroy ray is a genuine line trace — but it never ticks one. Every assertion is a mechanism
 * reading: the returned bool, the component's field, the structure's connection strengths, and the
 * presenter's own strings. Never a displacement; nothing here is released and nothing moves.
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

	/* --- ONE: the session opens on Auto, and the component agrees ---------------------------- */

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

	/* --- TWO: two bricks laid on Auto, bonded by the INFERRED perpend ------------------------ */

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

	/* --- THREE: the player picks Screw, and the chip reaches the component ------------------- */

	TestTrue(
		TEXT("the Screw chip must be clickable — a joint choice has no precondition"),
		Controller->OnToolbarButton(EToolbarButtonId::JointScrew));

	TestTrue(
		*FString::Printf(
			TEXT("the click must move the SESSION's own record of the choice; it reads %d"),
			static_cast<int32>(Controller->GetSessionToolbarState().Joint)),
		Controller->GetSessionToolbarState().Joint == EJointChoice::Screw);

	/*
	 * AND THE COMPONENT, WHICH IS THE HALF NO MODEL TEST CAN SEE. The state is what the strip
	 * draws; the component is what the next click lays. A controller that applied the transition
	 * and forgot the push would light the Screw chip over a plate that lands dry-bedded, and there
	 * is nothing on screen that would say so.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("and it must PUSH that choice onto the build component; the component reads %d"),
			static_cast<int32>(Build->JointChoice)),
		Build->JointChoice == EJointChoice::Screw);

	/* --- FOUR: a plate laid on that course is SCREWED to both bricks ------------------------- */

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

		/*
		 * THREE JOINTS: the course's own head joint, and ONE BEARING PER BRICK. The second bearing
		 * is the whole point — an override written onto the first joint alone leaves the far end of
		 * a plate the player screwed down resting on friction.
		 */
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

			/*
			 * AND THE WALL THE PLAYER ALREADY BUILT IS UNTOUCHED. The choice applies to the NEXT
			 * placement; a session that re-priced the existing joints would let a chip change a
			 * structure the player finished minutes ago, invisibly.
			 */
			CheckSessionProfile(
				*this,
				TEXT("the course's existing head joint is NOT re-priced by the later choice: "),
				Binding->GetStructure().GetConnection(0).Strength,
				GeneralPurposeMortarPerpend);
		}
	}

	/* --- FIVE: in Destroy mode, the details window NAMES what fastened it -------------------- */

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
			/*
			 * THE ROW SAYS "SCREW", AND THE REST OF THE SENTENCE IS THE PRESENTER'S. "screw",
			 * "Screw", "screwed", "screw joint" are all the answer a player needs; what may not
			 * happen is a row that names only its ROLE, because "bed below" is the same sentence
			 * for a screwed plate and a dry-bedded one — and those two structures behave nothing
			 * alike the moment either is run.
			 */
			TestTrue(
				*FString::Printf(
					TEXT("the screwed plate's joint row %d must NAME the profile that fastens it — it "
						 "reads '%s'"),
					Index, *Inspector.Joints[Index].Text),
				SaysWord(Inspector.Joints[Index].Text, TEXT("screw")));
		}

		/*
		 * AND THE DISCRIMINATION, WHICH IS WHAT STOPS "NAME THE PROFILE" BEING SATISFIED BY A
		 * CONSTANT. Brick 0 wears BOTH joints at once — the mortared head joint into its neighbour
		 * and the screwed bearing under the plate — so its two rows have to read differently. A
		 * presenter that printed "screw" on every row would pass the loop above and fail here.
		 *
		 * IT IS CLICKED INTO THE SELECTION FIRST, because BuildPieceMenuInspector singles out
		 * NOTHING for a ref that is not a member of the selection — deliberately, so a readout can
		 * never show somebody else's joints. Measured: without this click the rows came back empty
		 * and the two claims below failed for a fixture reason rather than a real one.
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
 * A JOINT ROW NAMES THE PROFILE THAT HOLDS IT, AND TWO JOINTS OF THE SAME ROLE CARRYING DIFFERENT
 * PROFILES READ DIFFERENTLY.
 *
 * =====================================================================================
 * WHY THIS IS SEPARABLE FROM THE SESSION TEST ABOVE
 * =====================================================================================
 *
 * `BuildPieceMenuInspector` is world-free — a binding in, strings out — so the claim about what a
 * row SAYS needs no controller, no actor and no trace. The session test proves the wire; this one
 * sweeps the whole vocabulary, which a session test cannot do without laying six differently
 * fastened pieces through a UI.
 *
 * =====================================================================================
 * SIX NEIGHBOURS, ONE PER FACE, AND WHY IT IS A SWEEP RATHER THAN A ROW ABOUT SCREWS
 * =====================================================================================
 *
 * A box has six faces and the toolbar offers six profiles, so the fixture is one brick surrounded
 * on every side, each joint carrying a different library row. That shape kills three ways of
 * passing without doing the work: a presenter printing a constant fails the distinctness claims; a
 * presenter naming the profile off the joint's ROLE fails on the two head joints, which share a
 * role and carry different profiles; and a presenter that only knows about the six choices the
 * override can produce still has to tell the strong bed mortar from its own weak perpend, which no
 * chip can select and every bonded wall in the game is full of.
 *
 * AND A SEVENTH CASE THE VOCABULARY CANNOT REACH: a strength this library never shipped, which must
 * read "custom" and must NOT be given the name of the shipped row it is nearest to. It gets a
 * fixture of its own below because a box has six faces and the sweep has taken all of them.
 *
 * THE WORDS ARE BOUNDED, NOT PINNED. The claim is that the row CONTAINS the fastener's word, not
 * that it reads any particular sentence — "screw", "Screw", "screwed" all say the same thing to a
 * player, and pinning the whole line would make every future retune a red test with nothing wrong
 * behind it. The library's own names are `GeneralPurposeMortar`, `GeneralPurposeMortarPerpend`,
 * `DryStone`, `Nail`, `Screw` and `Bolt`, and each row's required word is the distinguishing part
 * of its own name.
 *
 * THE PERPEND IS THE ONE ROW ALLOWED TO SAY "MORTAR" TOO, because its own library name contains
 * the word — it IS mortar, in a vertical joint, with its bond axes knocked down. What it may not
 * do is read as the FULL bed mortar, which is 4.5x its bond and the difference between a corner
 * that stands and one that does not.
 *
 * NEEDS A TICKING WORLD: no. Nothing is spawned and nothing is solved — a joint's profile is a fact
 * about the graph, not about a load, and the rows are built before anything has been settled.
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

		/**
		 * A word from another row that this row is nonetheless allowed to contain, or null.
		 *
		 * EXACTLY ONE EXEMPTION EXISTS AND IT IS NAMED RATHER THAN IMPLIED. A perpend is general
		 * purpose mortar in a vertical joint, so a row reading "mortar (perpend)" is honest; every
		 * other pair in this table names two genuinely different materials and a row that contained
		 * both would be lying about one of them.
		 */
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

	/*
	 * THE SUBJECT, THEN ONE NEIGHBOUR PER FACE. Handle 0 is the brick every row below belongs to;
	 * handles 1..6 are its neighbours in the order of the offsets, so case N's joint is the Nth
	 * connection and the mapping needs no searching.
	 */
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

		/*
		 * THE BOTTOM NEIGHBOUR IS THE GROUNDED ONE, so the fixture is a brick standing on something
		 * rather than a knot floating in space. Nothing here is solved, so it changes no number —
		 * it is what stops the fixture being a shape the solver would call stranded if anything
		 * ever did solve it.
		 */
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

		/*
		 * AND IT NAMES ONE PROFILE, NOT SEVERAL. A row that listed every word would contain its own
		 * and say nothing; a row that named the wrong sibling is worse than one that named none,
		 * because the number beside it is then the only thing that disagrees.
		 */
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

	/* --- AND A STRENGTH THIS LIBRARY NEVER SHIPPED READS "custom", NOT A PLAUSIBLE NEIGHBOUR --- */

	/*
	 * A SECOND FIXTURE RATHER THAN A SEVENTH FACE, and the reason is geometry: a box has six faces
	 * and the sweep above has taken all six. So the unshipped profile gets its own subject and one
	 * neighbour, which is the same claim on a smaller stage.
	 *
	 * WHY THE WORD MATTERS OVER A CASE NOTHING IN THE GAME BUILDS TODAY. Every joint the game makes
	 * comes from a library row, so this arm is unreachable through the UI — but it is exactly one
	 * authored joint, one retuned library or one save file away, and its failure mode is the quiet
	 * one: `FindConnectionProfileRow` answers null, and a presenter that dropped the word on null
	 * would print the ROLE-ONLY sentence this whole feature exists to end, with nothing to say a
	 * reading was missing. Naming the NEAREST row would be worse still — this library is siblings by
	 * construction, and a hundredth of a megapascal off a Screw is not a screw.
	 *
	 * 0.01 MPa OFF SCREW'S WITHDRAWAL, DELIBERATELY. It is as close to a shipped row as a value can
	 * be while not being it, so a lookup with any tolerance at all in it — or one that compares four
	 * of the five fields — names Screw here and this row catches it. A wildly different set of
	 * numbers would be found by nothing whatever the lookup did.
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
