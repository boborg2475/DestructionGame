// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

#include "Core/BuildMode/SnapSolver.h"
#include "Core/Connection.h"
#include "Core/PieceMenu.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/SessionToolbar.h"
#include "Core/Structure.h"
#include "Core/StructureBinding.h"
#include "DestructionGameFlyingPawn.h"
#include "DestructionGameGameMode.h"
#include "DestructionGamePlayerController.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/BrickActor.h"
#include "World/BuildModeComponent.h"
#include "World/DestructionScenarios.h"
#include "World/DestructionStructureSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Session tests. The toolbar (OnToolbarButton) is the only way into session state and also pushes
 * changes onto UBuildModeComponent; the mode decides what a ray does. SessionToolbar.h owns the
 * pure transition; this file pins the side effects. Mechanism readings only (DESIGN §4); no ticks.
 */
namespace SessionControllerTestSupport
{
	using namespace DestructionSession;

	/*
	 * Grid arithmetic spelled out, not imported (DESIGN §8). Course pitch 7.5 cm; a brick centres at
	 * n * 7.5 + 3.25, a plate (half height 5.0) at n * 7.5 + 5.0.
	 */

	/** Course 0's plane for a brick. */
	constexpr double SessionBrickPlaneCourse0Cm = 3.25;

	/** Course 2's plane for a plate. */
	constexpr double SessionPlatePlaneCourse2Cm = 20.0;

	/** Course 0's plane for a plate. */
	constexpr double SessionPlatePlaneCourse0Cm = 5.0;

	/** The demo timber plate's half extent, which SetPieceKind must derive. */
	const FVector SessionPlateHalfExtentCm(33.75, 5.125, 5.0);

	/**
	 * The run test's floating brick: plane 48.25, bottom at 45 cm, far from grounded and outside
	 * the 30 cm snap radius, so its Free placement is jointless.
	 */
	constexpr int32 SessionFloatingCourse = 6;
	constexpr double SessionFloatingPlaneZCm = 48.25;
	constexpr double SessionFloatingXCm = 100.0;

	/**
	 * Build-then-destroy poses. The second cursor at x 22.0 is 0.5 cm from the same-course pose at
	 * 22.5 and 13.1 cm from the nearest next-course one, so it snaps unambiguously.
	 */
	const FVector SessionSeedCentreCm(0.0, 0.0, SessionBrickPlaneCourse0Cm);
	constexpr double SessionSecondCursorXCm = 22.0;
	const FVector SessionSecondCentreCm(22.5, 0.0, SessionBrickPlaneCourse0Cm);

	/** A third, one more bay along, clear of the deleted brick at 0. */
	constexpr double SessionThirdCursorXCm = 45.0;

	/** X for the player's brick on a walled level, well clear of the sandbox row (-10.75..663.25). */
	constexpr double SessionClearOfTheWallXCm = 3000.0;

	/**
	 * Pointing-ray Z range. The end is at 0, not the plane, so a controller that used the end point
	 * instead of intersecting the plane would fail.
	 */
	constexpr double SessionRayStartZCm = 300.0;
	constexpr double SessionRayEndZCm = 0.0;

	/** Y reach of a Destroy ray either side of the brick. */
	constexpr double SessionInspectReachCm = 100.0;

	FVector SessionPointerRayStart(double XCm)
	{
		return FVector(XCm, 0.0, SessionRayStartZCm);
	}

	FVector SessionPointerRayEnd(double XCm)
	{
		return FVector(XCm, 0.0, SessionRayEndZCm);
	}

	// The same ray at any (X, Y), for corner legs running along Y.
	FVector SessionPointerRayStartAt(double XCm, double YCm)
	{
		return FVector(XCm, YCm, SessionRayStartZCm);
	}

	FVector SessionPointerRayEndAt(double XCm, double YCm)
	{
		return FVector(XCm, YCm, SessionRayEndZCm);
	}

	/** Whether the LP bridge poses this joint. It skips grounded-grounded joints, which get no readout. */
	bool SessionJointIsPosedByTheLP(const FStructure& Structure, int32 Connection)
	{
		const FConnection& Joint = Structure.GetConnection(Connection);

		return !(Structure.GetPiece(Joint.PieceA).bIsGrounded
			&& Structure.GetPiece(Joint.PieceB).bIsGrounded);
	}

	/** One joint on one line, for failure messages. */
	FString SessionDescribeJoint(const FStructure& Structure, int32 Connection)
	{
		const FConnection& Joint = Structure.GetConnection(Connection);
		const FStructure::FConnectionReadout Readout = Structure.GetConnectionReadout(Connection);

		return FString::Printf(
			TEXT("joint %d: %d-%d, n (%g, %g, %g), %.4f cm2, grounded %d/%d, posed %d, readout "
				 "present %d (N %g, util %g)"),
			Connection, Joint.PieceA, Joint.PieceB,
			Joint.InterfaceNormal.X, Joint.InterfaceNormal.Y, Joint.InterfaceNormal.Z,
			Joint.InterfaceAreaSqCm,
			Structure.GetPiece(Joint.PieceA).bIsGrounded ? 1 : 0,
			Structure.GetPiece(Joint.PieceB).bIsGrounded ? 1 : 0,
			SessionJointIsPosedByTheLP(Structure, Connection) ? 1 : 0,
			Readout.bPresent ? 1 : 0, Readout.NormalUu, Readout.Utilisation);
	}

	/** How many pieces the binding has released. */
	int32 SessionCountReleased(const FStructureBinding& Binding)
	{
		int32 Released = 0;

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			if (Binding.IsReleased(Piece))
			{
				++Released;
			}
		}

		return Released;
	}

	const TCHAR* SessionModeName(ESessionMode Mode)
	{
		switch (Mode)
		{
		case ESessionMode::Build:   return TEXT("Build");
		case ESessionMode::Destroy: return TEXT("Destroy");
		default:                    return TEXT("<unknown mode>");
		}
	}

	const TCHAR* SessionPieceName(EBuildPieceKind Kind)
	{
		switch (Kind)
		{
		case EBuildPieceKind::Brick:        return TEXT("Brick");
		case EBuildPieceKind::TimberPlate:  return TEXT("TimberPlate");
		case EBuildPieceKind::TimberLintel: return TEXT("TimberLintel");
		default:                            return TEXT("<unknown piece>");
		}
	}

	const TCHAR* SessionPlacementName(EPlacementMode Placement)
	{
		switch (Placement)
		{
		case EPlacementMode::Snap: return TEXT("Snap");
		case EPlacementMode::Free: return TEXT("Free");
		default:                   return TEXT("<unknown placement>");
		}
	}

	/** The whole session state on one line, so a failure reads without a debugger. */
	FString SessionStateBits(const FSessionToolbarState& State)
	{
		return FString::Printf(
			TEXT("{mode %s, piece %s, placement %s, course %d, hasStructure %d}"),
			SessionModeName(State.Mode), SessionPieceName(State.Piece),
			SessionPlacementName(State.Placement), State.Course, State.bHasStructure ? 1 : 0);
	}

	/** Field-wise equality; memcmp would compare padding. */
	bool SessionSameState(const FSessionToolbarState& A, const FSessionToolbarState& B)
	{
		return A.Mode == B.Mode
			&& A.Piece == B.Piece
			&& A.Placement == B.Placement
			&& A.Course == B.Course
			&& A.bHasStructure == B.bHasStructure
			&& A.bRotated == B.bRotated;
	}

	/** Whether the production strip draws this button enabled. Undrawn answers false. */
	bool SessionButtonIsEnabled(const FSessionToolbarState& State, EToolbarButtonId Id)
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);

		const FToolbarButton* const Button = Buttons.FindByPredicate(
			[Id](const FToolbarButton& Candidate) { return Candidate.Id == Id; });

		return Button != nullptr && Button->bEnabled;
	}

	/** ABrickActors in the world, excluding the ghost. */
	int32 SessionCountPlacedBricks(UWorld* World, const AActor* Ghost)
	{
		int32 Count = 0;

		for (TActorIterator<ABrickActor> It(World); It; ++It)
		{
			if (static_cast<const AActor*>(*It) != Ghost && IsValid(*It))
			{
				++Count;
			}
		}

		return Count;
	}

	/** The index of the presented row with this label, or INDEX_NONE. */
	int32 SessionFindMenuRow(TArrayView<const FPieceMenuRow> Rows, const TCHAR* Label)
	{
		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			if (Rows[Index].Label == FString(Label))
			{
				return Index;
			}
		}

		return INDEX_NONE;
	}

	FString SessionDescribeMenuRows(TArrayView<const FPieceMenuRow> Rows)
	{
		if (Rows.Num() == 0)
		{
			return TEXT("<no menu>");
		}

		FString Line;

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s'%s'->{%d,%d}"),
				Index == 0 ? TEXT("") : TEXT(", "),
				*Rows[Index].Label, Rows[Index].Ref.StructureId, Rows[Index].Ref.PieceIndex);
		}

		return Line;
	}

	/**
	 * A controller with a real ULocalPlayer (needed for the Enhanced Input mapping contexts) and
	 * its build component.
	 */
	struct FSessionFixture
	{
		BrickWorldTestSupport::FBrickTestWorld TestWorld;
		ADestructionGamePlayerController* Controller = nullptr;
		UBuildModeComponent* Build = nullptr;
		bool bWorldBegun = false;

		bool Begin(FAutomationTestBase& Test)
		{
			if (!TestWorld.Begin(Test))
			{
				return false;
			}

			bWorldBegun = true;

			Controller = BrickWorldTestSupport::SpawnControllerWithLocalPlayer(Test, TestWorld.World);

			if (Controller == nullptr)
			{
				return false;
			}

			Build = Controller->GetBuildComponent();

			Test.TestNotNull(
				TEXT("fixture: a spawned controller must already carry its UBuildModeComponent — it is "
					 "a default subobject so that nothing has to remember to create one"),
				Build);

			return Build != nullptr;
		}

		void End()
		{
			if (bWorldBegun)
			{
				TestWorld.End();
				bWorldBegun = false;
			}
		}
	};

	/**
	 * Spawn a controller and pawn before begin-play, as UEngine::LoadMap does. No local player:
	 * attaching one here triggers a viewport-less ensure.
	 */
	void SessionSpawnPlayer(UWorld& World, ADestructionGamePlayerController*& OutController)
	{
		OutController = World.SpawnActor<ADestructionGamePlayerController>();

		APawn* const Pawn = World.SpawnActor<ADestructionGameFlyingPawn>(
			FVector(-5000.0, -5000.0, -500.0), FRotator::ZeroRotator);

		if (OutController != nullptr && Pawn != nullptr)
		{
			OutController->Possess(Pawn);
		}
	}
}

/**
 * Every accepted toolbar click moves both the state and the component; every refused click moves
 * neither.
 *
 * The controller opens in Destroy, since most levels lay a structure to pull apart. Three refusals
 * each catch a different mistake: Course down at 0, Run (not on the Build strip), and Clear with
 * nothing laid (checked by the unchanged structure id).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarDrivesTheSessionTest,
	"DestructionGame.World.Session.ToolbarDrivesTheSession",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarDrivesTheSessionTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionProfiles;
	using namespace DestructionSession;
	using namespace SessionControllerTestSupport;

	FSessionFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;
	UBuildModeComponent& Build = *Fixture.Build;
	UDestructionStructureSubsystem& Subsystem = *Fixture.TestWorld.Subsystem;

	// One: the opening state.

	{
		const FSessionToolbarState& State = Controller.GetSessionToolbarState();

		AddInfo(FString::Printf(
			TEXT("a fresh controller's session state is %s"), *SessionStateBits(State)));

		TestTrue(
			*FString::Printf(
				TEXT("A SCENARIO LEVEL OPENS IN DESTROY: a controller that opened in Build would put a "
					 "ghost over the wall the level just laid and swallow the first click. The state "
					 "is %s"),
				*SessionStateBits(State)),
			State.Mode == ESessionMode::Destroy);

		TestTrue(
			*FString::Printf(
				TEXT("and the build settings start where the model starts — Brick, Snap, course 0, and "
					 "no structure to command. The state is %s"),
				*SessionStateBits(State)),
			State.Piece == EBuildPieceKind::Brick
				&& State.Placement == EPlacementMode::Snap
				&& State.Course == 0
				&& State.bHasStructure == false);

		TestEqual(
			FString::Printf(
				TEXT("nothing has been built and no level laid anything, so the session names no "
					 "structure; it names %d"),
				Controller.GetSessionStructureId()),
			Controller.GetSessionStructureId(), static_cast<int32>(INDEX_NONE));

		TestEqual(
			FString::Printf(
				TEXT("and the build component has no build open either; it holds %d"),
				Build.GetStructureId()),
			Build.GetStructureId(), static_cast<int32>(INDEX_NONE));
	}

	// Two: Build opens a build and shows the cursor.

	{
		const bool bSwitched = Controller.OnToolbarButton(EToolbarButtonId::ModeBuild);

		TestTrue(
			FString::Printf(
				TEXT("the Build tab is always live, so clicking it must report that it landed; it "
					 "reported %d"),
				bSwitched ? 1 : 0),
			bSwitched);

		TestTrue(
			*FString::Printf(
				TEXT("clicking Build must put the session in Build mode; it is %s"),
				*SessionStateBits(Controller.GetSessionToolbarState())),
			Controller.GetSessionToolbarState().Mode == ESessionMode::Build);

		// Without an open build every click would fail closed.
		TestTrue(
			FString::Printf(
				TEXT("entering Build must OPEN a build, so the component names a real structure; it "
					 "names %d"),
				Build.GetStructureId()),
			Build.GetStructureId() != INDEX_NONE);

		TestNotNull(
			*FString::Printf(
				TEXT("and the subsystem must actually hold structure %d"), Build.GetStructureId()),
			Subsystem.Find(Build.GetStructureId()));

		// Raised in BeginPlay since S6; the mode switch must leave it up (see Nine).
		TestTrue(
			TEXT("Build mode must show the mouse cursor — there is no aiming a ghost with a camera "
				 "that follows every mouse movement"),
			Controller.bShowMouseCursor);
	}

	const int32 BuildStructureId = Build.GetStructureId();

	// Three: the piece click derives extent and plane.

	{
		const bool bChosePlate = Controller.OnToolbarButton(EToolbarButtonId::PieceTimberPlate);

		TestTrue(
			FString::Printf(
				TEXT("a piece button is always live in Build mode; the plate reported %d"),
				bChosePlate ? 1 : 0),
			bChosePlate);

		TestTrue(
			*FString::Printf(
				TEXT("the state must record the plate; it is %s"),
				*SessionStateBits(Controller.GetSessionToolbarState())),
			Controller.GetSessionToolbarState().Piece == EBuildPieceKind::TimberPlate);

		TestTrue(
			FString::Printf(
				TEXT("AND THE COMPONENT MUST HAVE BEEN TOLD: it reads piece kind %s"),
				SessionPieceName(Build.GetPieceKind())),
			Build.GetPieceKind() == EBuildPieceKind::TimberPlate);

		TestTrue(
			FString::Printf(
				TEXT("which derives the demo's plate half extent (33.75, 5.125, 5); it reads "
					 "(%g, %g, %g)"),
				Build.CurrentExtentCm.X, Build.CurrentExtentCm.Y, Build.CurrentExtentCm.Z),
			Build.CurrentExtentCm.Equals(SessionPlateHalfExtentCm, KINDA_SMALL_NUMBER));

		TestEqual(
			FString::Printf(
				TEXT("and the plate on course 0 planes at 0 * 7.5 + 5 = 5; it is %g"),
				Build.BuildPlaneZCm),
			Build.BuildPlaneZCm, SessionPlatePlaneCourse0Cm);
	}

	// Four: Snap/Free reaches the component.

	{
		const bool bChoseFree = Controller.OnToolbarButton(EToolbarButtonId::PlacementFree);

		TestTrue(
			FString::Printf(TEXT("the Free button is always live; it reported %d"), bChoseFree ? 1 : 0),
			bChoseFree);

		TestTrue(
			*FString::Printf(
				TEXT("the state must record Free placement; it is %s"),
				*SessionStateBits(Controller.GetSessionToolbarState())),
			Controller.GetSessionToolbarState().Placement == EPlacementMode::Free);

		TestTrue(
			FString::Printf(
				TEXT("AND THE COMPONENT MUST HAVE BEEN TOLD — it is the field the subsystem reads to "
					 "honour the cursor verbatim; it reads %s"),
				SessionPlacementName(Build.PlacementMode)),
			Build.PlacementMode == EPlacementMode::Free);
	}

	// Five: the course stepper moves the build plane.

	{
		TestTrue(
			TEXT("Course up is always live, so the first step must land"),
			Controller.OnToolbarButton(EToolbarButtonId::CourseUp));

		TestTrue(
			TEXT("and so must the second"),
			Controller.OnToolbarButton(EToolbarButtonId::CourseUp));

		TestEqual(
			FString::Printf(
				TEXT("two steps up put the session on course 2; it reads %d"),
				Controller.GetSessionToolbarState().Course),
			Controller.GetSessionToolbarState().Course, 2);

		TestEqual(
			FString::Printf(
				TEXT("AND THE COMPONENT WITH IT; it reads course %d"), Build.GetCourse()),
			Build.GetCourse(), 2);

		// The plate's plane, not the brick's: the course push must re-derive it.
		TestEqual(
			FString::Printf(
				TEXT("and the plate on course 2 planes at 2 * 7.5 + 5 = 20 (NOT the brick's 18.25); it "
					 "is %g"),
				Build.BuildPlaneZCm),
			Build.BuildPlaneZCm, SessionPlatePlaneCourse2Cm);
	}

	// Six: down to course 0, then refused.

	{
		TestTrue(
			TEXT("Course down is live at course 2"),
			Controller.OnToolbarButton(EToolbarButtonId::CourseDown));

		TestTrue(
			TEXT("and at course 1"),
			Controller.OnToolbarButton(EToolbarButtonId::CourseDown));

		TestEqual(
			FString::Printf(
				TEXT("two steps down return the session to course 0; it reads %d"),
				Controller.GetSessionToolbarState().Course),
			Controller.GetSessionToolbarState().Course, 0);

		const FSessionToolbarState Before = Controller.GetSessionToolbarState();

		const bool bSteppedBelow = Controller.OnToolbarButton(EToolbarButtonId::CourseDown);

		TestTrue(
			FString::Printf(
				TEXT("THERE IS NO COURSE BELOW THE ONE WITH THE EARTH UNDER IT: a third step down must "
					 "report that it did nothing; it reported %d"),
				bSteppedBelow ? 1 : 0),
			!bSteppedBelow);

		TestTrue(
			*FString::Printf(
				TEXT("and it must leave the state exactly as it was — %s against %s"),
				*SessionStateBits(Controller.GetSessionToolbarState()), *SessionStateBits(Before)),
			SessionSameState(Controller.GetSessionToolbarState(), Before));

		TestEqual(
			FString::Printf(
				TEXT("AND THE COMPONENT UNTOUCHED: a refused click that still pushed its own decrement "
					 "would put the build plane under the earth. It reads course %d"),
				Build.GetCourse()),
			Build.GetCourse(), 0);

		TestEqual(
			FString::Printf(
				TEXT("with the plate's course-0 plane of 5, not a plane below the ground; it is %g"),
				Build.BuildPlaneZCm),
			Build.BuildPlaneZCm, SessionPlatePlaneCourse0Cm);
	}

	// Seven: a button the strip does not draw.

	{
		const FSessionToolbarState Before = Controller.GetSessionToolbarState();

		const bool bRan = Controller.OnToolbarButton(EToolbarButtonId::RunStructure);

		TestTrue(
			FString::Printf(
				TEXT("RUN IS NOT ON THE BUILD STRIP AT ALL, so clicking it — from a stale widget or a "
					 "keybind — must report that it did nothing; it reported %d"),
				bRan ? 1 : 0),
			!bRan);

		TestTrue(
			*FString::Printf(
				TEXT("and change nothing: %s against %s"),
				*SessionStateBits(Controller.GetSessionToolbarState()), *SessionStateBits(Before)),
			SessionSameState(Controller.GetSessionToolbarState(), Before));

		if (FStructureBinding* const Binding = Subsystem.Find(BuildStructureId))
		{
			TestEqual(
				FString::Printf(
					TEXT("nor may it have solved or settled anything: the build holds %d piece(s) and "
						 "%d released"),
					Binding->NumPieces(), Binding->GetStructure().NumPieces()),
				Binding->NumPieces(), 0);
		}
	}

	// Eight: a command whose precondition is not met.

	{
		TestTrue(
			*FString::Printf(
				TEXT("fixture: nothing has been laid, so the strip must be greying Clear build. The "
					 "state is %s"),
				*SessionStateBits(Controller.GetSessionToolbarState())),
			!SessionButtonIsEnabled(Controller.GetSessionToolbarState(), EToolbarButtonId::ClearBuild));

		const FSessionToolbarState Before = Controller.GetSessionToolbarState();

		const bool bCleared = Controller.OnToolbarButton(EToolbarButtonId::ClearBuild);

		TestTrue(
			FString::Printf(
				TEXT("CLEARING AN EMPTY PLOT CLEARS NOTHING, and a silent no-op on a command button "
					 "reads as a missed click — so it must report that it did nothing; it reported %d"),
				bCleared ? 1 : 0),
			!bCleared);

		TestTrue(
			*FString::Printf(
				TEXT("and change nothing: %s against %s"),
				*SessionStateBits(Controller.GetSessionToolbarState()), *SessionStateBits(Before)),
			SessionSameState(Controller.GetSessionToolbarState(), Before));

		// Clear would change the id; the piece count is zero either way.
		TestEqual(
			FString::Printf(
				TEXT("and above all leave the build it already had open: structure %d, not a fresh "
					 "one; the component holds %d"),
				BuildStructureId, Build.GetStructureId()),
			Build.GetStructureId(), BuildStructureId);
	}

	// Nine: the cursor belongs to the session, not the mode (SESSION_UI_DESIGN §d, S6).
	{
		TestTrue(
			TEXT("the Destroy tab is always live"),
			Controller.OnToolbarButton(EToolbarButtonId::ModeDestroy));

		TestTrue(
			*FString::Printf(
				TEXT("DESTROY MODE KEEPS THE CURSOR. It is the session's pointer, not Build mode's — "
					 "the strip is on screen in both modes and hovering a brick needs it as much as "
					 "aiming a ghost does. The state is %s"),
				*SessionStateBits(Controller.GetSessionToolbarState())),
			Controller.bShowMouseCursor);

		TestTrue(
			TEXT("and back in Build it is still up — this is not a toggle that happens to be true "
				 "in one mode"),
			Controller.OnToolbarButton(EToolbarButtonId::ModeBuild) && Controller.bShowMouseCursor);
	}

	Fixture.End();

	return true;
}

/**
 * In Build a click lays a brick, in Destroy the same click opens that brick's menu, and the build
 * survives the round trip.
 *
 * One test because the claim is a loop (lay, destroy, lay again); split, each half would pass a
 * controller that reset the build on every mode switch. The ghost is checked for visibility only;
 * its position is pinned in World.BuildMode.ComponentRayDrivesPreviewAndGhost.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionPrimaryRayPlacesAndInspectsTest,
	"DestructionGame.World.Session.PrimaryRayPlacesInBuildAndInspectsInDestroy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionPrimaryRayPlacesAndInspectsTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionControllerTestSupport;

	FSessionFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;
	UBuildModeComponent& Build = *Fixture.Build;
	UDestructionStructureSubsystem& Subsystem = *Fixture.TestWorld.Subsystem;

	if (!Controller.OnToolbarButton(EToolbarButtonId::ModeBuild))
	{
		AddError(TEXT("fixture: the Build tab must be clickable for any of this to run"));
		Fixture.End();
		return true;
	}

	const int32 BuildStructureId = Build.GetStructureId();

	// One: pointing in Build mode drives the ghost.

	Controller.PointerAlongRay(
		SessionPointerRayStart(0.0), SessionPointerRayEnd(0.0));

	AActor* const Ghost = Build.GetGhostActor();

	TestNotNull(
		TEXT("pointing in Build mode must reach the build component's preview, which spawns its ghost"),
		Ghost);

	if (Ghost != nullptr)
	{
		TestFalse(
			TEXT("and a ray that meets the build plane must SHOW the ghost — the player has to see "
				 "where the click will land before they make it"),
			Ghost->IsHidden());
	}

	// Two: a Build click lays a brick and opens no menu.

	{
		const bool bPlaced = Controller.PrimaryAlongRay(
			SessionPointerRayStart(0.0), SessionPointerRayEnd(0.0));

		TestTrue(
			FString::Printf(
				TEXT("a primary click on the build plane must report that a piece landed; it reported "
					 "%d"),
				bPlaced ? 1 : 0),
			bPlaced);

		FStructureBinding* const Binding = Subsystem.Find(BuildStructureId);

		if (Binding == nullptr)
		{
			AddError(FString::Printf(
				TEXT("the build structure %d vanished under the first click"), BuildStructureId));

			Fixture.End();
			return true;
		}

		TestEqual(
			FString::Printf(
				TEXT("the click must grow the build to exactly one piece; it holds %d"),
				Binding->NumPieces()),
			Binding->NumPieces(), 1);

		if (Binding->NumPieces() == 1)
		{
			const FVector CentreCm = Binding->GetBinding(0).Box.CentreCm;

			TestTrue(
				FString::Printf(
					TEXT("and lay it on the course-0 plane the ray met, (0, 0, 3.25); it is "
						 "(%g, %g, %g)"),
					CentreCm.X, CentreCm.Y, CentreCm.Z),
				CentreCm.Equals(SessionSeedCentreCm, KINDA_SMALL_NUMBER));

			// Bottom face at Z = 0, within the 1 cm grounding tolerance.
			TestTrue(
				TEXT("the first brick rests ON the earth, so the structure must record it grounded — a "
					 "foundation that is not grounded routes its load nowhere"),
				Binding->GetStructure().GetPiece(0).bIsGrounded);
		}

		TestTrue(
			*FString::Printf(
				TEXT("AND NO PIECE MENU MAY APPEAR IN BUILD MODE — a Delete menu over the brick just "
					 "laid, with the cursor captured, is today's click leaking through. It shows [%s]"),
				*SessionDescribeMenuRows(Controller.GetShownPieceMenuRows())),
			!Controller.IsPieceMenuShown());
	}

	// Three: a second click, snapped onto the bond.

	{
		const bool bPlaced = Controller.PrimaryAlongRay(
			SessionPointerRayStart(SessionSecondCursorXCm),
			SessionPointerRayEnd(SessionSecondCursorXCm));

		TestTrue(
			FString::Printf(
				TEXT("the second click must land a piece too; it reported %d"), bPlaced ? 1 : 0),
			bPlaced);

		FStructureBinding* const Binding = Subsystem.Find(BuildStructureId);

		if (Binding == nullptr || Binding->NumPieces() != 2)
		{
			AddError(FString::Printf(
				TEXT("the second click must grow the build to 2 pieces; it holds %d"),
				Binding != nullptr ? Binding->NumPieces() : INDEX_NONE));

			Fixture.End();
			return true;
		}

		const FVector CentreCm = Binding->GetBinding(1).Box.CentreCm;

		TestTrue(
			FString::Printf(
				TEXT("a cursor 0.5 cm off the bond must be PULLED onto it — (22.5, 0, 3.25), not the "
					 "(22, 0, 3.25) that was asked for; it is (%g, %g, %g)"),
				CentreCm.X, CentreCm.Y, CentreCm.Z),
			CentreCm.Equals(SessionSecondCentreCm, KINDA_SMALL_NUMBER));

		// One connection: the mechanism reading of "it bonded".
		TestEqual(
			FString::Printf(
				TEXT("and form the head joint between the two, so the structure holds one connection; "
					 "it holds %d"),
				Binding->GetStructure().NumConnections()),
			Binding->GetStructure().NumConnections(), 1);

		TestTrue(
			TEXT("still no piece menu after a second Build-mode click"),
			!Controller.IsPieceMenuShown());
	}

	// Four: the strip now knows there is a structure.

	{
		const FSessionToolbarState& State = Controller.GetSessionToolbarState();

		TestTrue(
			*FString::Printf(
				TEXT("TWO BRICKS ARE A STRUCTURE: the session state must say so, or the strip greys "
					 "Clear and Run over a build the player can see. It reads %s"),
				*SessionStateBits(State)),
			State.bHasStructure);

		TestTrue(
			*FString::Printf(
				TEXT("and Clear build must therefore be drawn LIVE; the state is %s"),
				*SessionStateBits(State)),
			SessionButtonIsEnabled(State, EToolbarButtonId::ClearBuild));
	}

	// Five: leaving Build hides the ghost.

	{
		TestTrue(
			TEXT("the Destroy tab is always live"),
			Controller.OnToolbarButton(EToolbarButtonId::ModeDestroy));

		TestTrue(
			*FString::Printf(
				TEXT("the session must be in Destroy mode; it reads %s"),
				*SessionStateBits(Controller.GetSessionToolbarState())),
			Controller.GetSessionToolbarState().Mode == ESessionMode::Destroy);

		if (Ghost != nullptr)
		{
			TestTrue(
				TEXT("NO GHOST SURVIVES INTO DESTROY MODE — a gold brick hanging in the air over a "
					 "wall the player is demolishing is the most confusing thing this UI can do"),
				Ghost->IsHidden());
		}
	}

	// Six: pointing in Destroy highlights the brick under the cursor.

	FStructureBinding* Binding = Subsystem.Find(BuildStructureId);

	if (Binding == nullptr || Binding->NumPieces() != 2)
	{
		AddError(TEXT("the build must still hold its two bricks going into Destroy mode"));
		Fixture.End();
		return true;
	}

	ABrickActor* const FirstBrick = Cast<ABrickActor>(Binding->GetActor(0));
	ABrickActor* const SecondBrick = Cast<ABrickActor>(Binding->GetActor(1));

	TestNotNull(TEXT("fixture: the first laid piece must be backed by an ABrickActor"), FirstBrick);
	TestNotNull(TEXT("fixture: the second laid piece must be backed by an ABrickActor"), SecondBrick);

	if (FirstBrick == nullptr || SecondBrick == nullptr)
	{
		Fixture.End();
		return true;
	}

	const FVector InspectStart(
		SessionSeedCentreCm.X, SessionSeedCentreCm.Y - SessionInspectReachCm, SessionSeedCentreCm.Z);

	const FVector InspectEnd(
		SessionSeedCentreCm.X, SessionSeedCentreCm.Y + SessionInspectReachCm, SessionSeedCentreCm.Z);

	{
		Controller.PointerAlongRay(InspectStart, InspectEnd);

		TestTrue(
			FString::Printf(
				TEXT("pointing in Destroy mode must CALL OUT the brick under the cursor, which is what "
					 "tells a player what a click would take; brick 0 reads highlight %d against "
					 "Hovered (%d)"),
				static_cast<int32>(FirstBrick->GetHighlight()),
				static_cast<int32>(EBrickHighlight::Hovered)),
			FirstBrick->GetHighlight() == EBrickHighlight::Hovered);

		TestTrue(
			TEXT("and a hover must open no menu — pointing is not clicking"),
			!Controller.IsPieceMenuShown());
	}

	// Seven: a Destroy click opens that brick's menu.

	int32 DeleteRow = INDEX_NONE;

	{
		Controller.PrimaryAlongRay(InspectStart, InspectEnd);

		const TArrayView<const FPieceMenuRow> Rows = Controller.GetShownPieceMenuRows();

		TestTrue(
			*FString::Printf(
				TEXT("A PRIMARY CLICK IN DESTROY MODE IS TODAY'S CLICK: it must put the pointed-at "
					 "brick's menu up. It shows [%s]"),
				*SessionDescribeMenuRows(Rows)),
			Controller.IsPieceMenuShown());

		DeleteRow = SessionFindMenuRow(Rows, TEXT("Delete"));

		TestTrue(
			*FString::Printf(
				TEXT("and that menu must offer Delete against the brick the ray hit; it shows [%s]"),
				*SessionDescribeMenuRows(Rows)),
			DeleteRow != INDEX_NONE
				&& Rows.IsValidIndex(DeleteRow)
				&& Rows[DeleteRow].Ref.StructureId == BuildStructureId
				&& Rows[DeleteRow].Ref.PieceIndex == 0);
	}

	if (DeleteRow == INDEX_NONE)
	{
		Fixture.End();
		return true;
	}

	// Eight: Delete takes that brick and only that brick.

	{
		const bool bChose = Controller.ChoosePieceMenuRow(DeleteRow);

		TestTrue(
			FString::Printf(
				TEXT("choosing Delete must report that it committed; it reported %d"), bChose ? 1 : 0),
			bChose);

		Binding = Subsystem.Find(BuildStructureId);

		if (Binding == nullptr)
		{
			AddError(TEXT("the build structure vanished under the delete"));
			Fixture.End();
			return true;
		}

		TestTrue(
			FString::Printf(
				TEXT("the chosen piece must leave the graph; IsPieceRemoved(0) reports %d"),
				Binding->IsPieceRemoved(0) ? 1 : 0),
			Binding->IsPieceRemoved(0));

		TestTrue(
			FString::Printf(
				TEXT("and its actor must leave the world; it is %s"),
				IsValid(FirstBrick) ? TEXT("still valid") : TEXT("gone")),
			!IsValid(FirstBrick));

		TestTrue(
			FString::Printf(
				TEXT("the brick that was NOT chosen must still stand: IsPieceRemoved(1) reports %d and "
					 "its actor is %s"),
				Binding->IsPieceRemoved(1) ? 1 : 0,
				IsValid(SecondBrick) ? TEXT("valid") : TEXT("gone")),
			!Binding->IsPieceRemoved(1) && IsValid(SecondBrick));

		TestEqual(
			FString::Printf(
				TEXT("so exactly one piece is live; %d are"),
				Binding->GetStructure().NumLivePieces()),
			Binding->GetStructure().NumLivePieces(), 1);
	}

	// Nine: back to Build, the same build (not cancelled on the mode switch).
	{
		TestTrue(
			TEXT("the Build tab is always live"),
			Controller.OnToolbarButton(EToolbarButtonId::ModeBuild));

		TestEqual(
			FString::Printf(
				TEXT("RETURNING TO BUILD MUST CONTINUE THE SAME BUILD, not start a new one: structure "
					 "%d, the component holds %d"),
				BuildStructureId, Build.GetStructureId()),
			Build.GetStructureId(), BuildStructureId);

		const bool bPlaced = Controller.PrimaryAlongRay(
			SessionPointerRayStart(SessionThirdCursorXCm),
			SessionPointerRayEnd(SessionThirdCursorXCm));

		TestTrue(
			FString::Printf(
				TEXT("and a click must lay a third brick onto it; it reported %d"), bPlaced ? 1 : 0),
			bPlaced);

		Binding = Subsystem.Find(BuildStructureId);

		if (Binding != nullptr)
		{
			TestEqual(
				FString::Printf(
					TEXT("the structure must have grown by one piece — 3 laid in all; it holds %d"),
					Binding->NumPieces()),
				Binding->NumPieces(), 3);

			TestEqual(
				FString::Printf(
					TEXT("two of which are live (one was deleted); %d are"),
					Binding->GetStructure().NumLivePieces()),
				Binding->GetStructure().NumLivePieces(), 2);
		}
	}

	Fixture.End();

	return true;
}

/**
 * Run structure settles the build: the floating brick is released, the grounded one is not.
 *
 * The floating brick is a jointless Free placement six courses up. The grounded seed staying
 * kinematic rules out "release everything". Asserted on IsReleased, never displacement (DESIGN §4).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionRunStructureSettlesTheBuildTest,
	"DestructionGame.World.Session.RunStructureSettlesTheBuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionRunStructureSettlesTheBuildTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionControllerTestSupport;

	FSessionFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;
	UBuildModeComponent& Build = *Fixture.Build;
	UDestructionStructureSubsystem& Subsystem = *Fixture.TestWorld.Subsystem;

	if (!Controller.OnToolbarButton(EToolbarButtonId::ModeBuild))
	{
		AddError(TEXT("fixture: the Build tab must be clickable for any of this to run"));
		Fixture.End();
		return true;
	}

	const int32 BuildStructureId = Build.GetStructureId();

	// The grounded seed on course 0.
	Controller.PrimaryAlongRay(SessionPointerRayStart(0.0), SessionPointerRayEnd(0.0));

	// The floating brick: Free, six courses up, 100 cm away.
	for (int32 Step = 0; Step < SessionFloatingCourse; ++Step)
	{
		Controller.OnToolbarButton(EToolbarButtonId::CourseUp);
	}

	TestEqual(
		FString::Printf(
			TEXT("fixture: six steps up put the session on course %d; it reads %d"),
			SessionFloatingCourse, Controller.GetSessionToolbarState().Course),
		Controller.GetSessionToolbarState().Course, SessionFloatingCourse);

	TestEqual(
		FString::Printf(
			TEXT("fixture: which planes a brick at %d * 7.5 + 3.25 = %g; the component reads %g"),
			SessionFloatingCourse, SessionFloatingPlaneZCm, Build.BuildPlaneZCm),
		Build.BuildPlaneZCm, SessionFloatingPlaneZCm);

	Controller.OnToolbarButton(EToolbarButtonId::PlacementFree);

	Controller.PrimaryAlongRay(
		SessionPointerRayStart(SessionFloatingXCm), SessionPointerRayEnd(SessionFloatingXCm));

	FStructureBinding* Binding = Subsystem.Find(BuildStructureId);

	if (Binding == nullptr || Binding->NumPieces() != 2)
	{
		AddError(FString::Printf(
			TEXT("fixture: the two placements must give a two-piece build; it holds %d"),
			Binding != nullptr ? Binding->NumPieces() : INDEX_NONE));

		Fixture.End();
		return true;
	}

	// Preconditions: one piece that must fall, one that must not.

	TestTrue(
		TEXT("fixture: the seed must be grounded, or 'it did not fall' says nothing about the solver"),
		Binding->GetStructure().GetPiece(0).bIsGrounded);

	TestTrue(
		FString::Printf(
			TEXT("fixture: the Free brick must NOT be grounded — its bottom face is at %g cm — or "
				 "nothing in this build can fall"),
			SessionFloatingPlaneZCm - 3.25),
		!Binding->GetStructure().GetPiece(1).bIsGrounded);

	TestEqual(
		FString::Printf(
			TEXT("fixture: and it must be bonded to nothing, so the build holds no connections at "
				 "all; it holds %d"),
			Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), 0);

	TestTrue(
		FString::Printf(
			TEXT("fixture: nothing may be released before Run — piece 0 reads %d, piece 1 reads %d"),
			Binding->IsReleased(0) ? 1 : 0, Binding->IsReleased(1) ? 1 : 0),
		!Binding->IsReleased(0) && !Binding->IsReleased(1));

	// Run, from Destroy mode, through the toolbar.

	TestTrue(
		TEXT("the Destroy tab is always live"),
		Controller.OnToolbarButton(EToolbarButtonId::ModeDestroy));

	{
		const FSessionToolbarState& State = Controller.GetSessionToolbarState();

		TestTrue(
			*FString::Printf(
				TEXT("fixture: with a two-piece build open the strip must offer Run structure LIVE, or "
					 "the click below is refused for the wrong reason. The state is %s"),
				*SessionStateBits(State)),
			SessionButtonIsEnabled(State, EToolbarButtonId::RunStructure));
	}

	TestEqual(
		FString::Printf(
			TEXT("fixture: the session must run the PLAYER'S build, structure %d; it names %d"),
			BuildStructureId, Controller.GetSessionStructureId()),
		Controller.GetSessionStructureId(), BuildStructureId);

	const bool bRan = Controller.OnToolbarButton(EToolbarButtonId::RunStructure);

	TestTrue(
		FString::Printf(
			TEXT("clicking Run on a live build must report that it landed; it reported %d"),
			bRan ? 1 : 0),
		bRan);

	Binding = Subsystem.Find(BuildStructureId);

	if (Binding == nullptr)
	{
		AddError(TEXT("the build structure vanished under Run"));
		Fixture.End();
		return true;
	}

	TestTrue(
		FString::Printf(
			TEXT("RUN MUST SETTLE THE BUILD: the brick with nothing under it has to be handed to "
				 "physics. IsReleased(1) reports %d"),
			Binding->IsReleased(1) ? 1 : 0),
		Binding->IsReleased(1));

	TestTrue(
		FString::Printf(
			TEXT("and the one resting on the earth must NOT be — a Run that released everything would "
				 "satisfy the row above and drop the player's foundation. IsReleased(0) reports %d"),
			Binding->IsReleased(0) ? 1 : 0),
		!Binding->IsReleased(0));

	Fixture.End();

	return true;
}

/**
 * Clear build empties the plot and opens a fresh one, staying in Build mode.
 *
 * Both the world's bricks and the binding are checked, since either can be left behind. The fresh
 * build must have a different id, not merely a valid one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionClearBuildEmptiesThePlotTest,
	"DestructionGame.World.Session.ClearBuildEmptiesThePlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionClearBuildEmptiesThePlotTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionControllerTestSupport;

	FSessionFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;
	UBuildModeComponent& Build = *Fixture.Build;
	UDestructionStructureSubsystem& Subsystem = *Fixture.TestWorld.Subsystem;

	if (!Controller.OnToolbarButton(EToolbarButtonId::ModeBuild))
	{
		AddError(TEXT("fixture: the Build tab must be clickable for any of this to run"));
		Fixture.End();
		return true;
	}

	const int32 BuildStructureId = Build.GetStructureId();

	Controller.PrimaryAlongRay(SessionPointerRayStart(0.0), SessionPointerRayEnd(0.0));

	Controller.PrimaryAlongRay(
		SessionPointerRayStart(SessionSecondCursorXCm),
		SessionPointerRayEnd(SessionSecondCursorXCm));

	{
		FStructureBinding* const Binding = Subsystem.Find(BuildStructureId);

		if (Binding == nullptr || Binding->NumPieces() != 2)
		{
			AddError(FString::Printf(
				TEXT("fixture: the two clicks must give a two-piece build; it holds %d"),
				Binding != nullptr ? Binding->NumPieces() : INDEX_NONE));

			Fixture.End();
			return true;
		}
	}

	const int32 BricksBefore =
		SessionCountPlacedBricks(Fixture.TestWorld.World, Build.GetGhostActor());

	TestEqual(
		FString::Printf(
			TEXT("fixture: two bricks must be standing on the plot before it is cleared; %d are"),
			BricksBefore),
		BricksBefore, 2);

	const bool bCleared = Controller.OnToolbarButton(EToolbarButtonId::ClearBuild);

	TestTrue(
		FString::Printf(
			TEXT("Clear build over a live build must report that it landed; it reported %d"),
			bCleared ? 1 : 0),
		bCleared);

	const int32 BricksAfter =
		SessionCountPlacedBricks(Fixture.TestWorld.World, Build.GetGhostActor());

	TestEqual(
		FString::Printf(
			TEXT("CLEAR MEANS THE PLOT IS EMPTY: not one brick may be left standing, or the player is "
				 "looking at colliders nothing in the model knows about. %d remain"),
			BricksAfter),
		BricksAfter, 0);

	TestNull(
		*FString::Printf(
			TEXT("and the cleared structure %d must be gone from the subsystem"), BuildStructureId),
		Subsystem.Find(BuildStructureId));

	// And a fresh plot is open.

	TestTrue(
		FString::Printf(
			TEXT("A FRESH BUILD MUST BE OPEN — clearing is 'start again', not 'stop building'. The "
				 "component holds %d, which must be a real id and not the cleared %d"),
			Build.GetStructureId(), BuildStructureId),
		Build.GetStructureId() != INDEX_NONE && Build.GetStructureId() != BuildStructureId);

	TestNotNull(
		*FString::Printf(
			TEXT("and the subsystem must hold that fresh structure %d"), Build.GetStructureId()),
		Subsystem.Find(Build.GetStructureId()));

	if (FStructureBinding* const Fresh = Subsystem.Find(Build.GetStructureId()))
	{
		TestEqual(
			FString::Printf(TEXT("empty, of course; it holds %d piece(s)"), Fresh->NumPieces()),
			Fresh->NumPieces(), 0);
	}

	{
		const FSessionToolbarState& State = Controller.GetSessionToolbarState();

		TestTrue(
			*FString::Printf(
				TEXT("and the strip must know the plot is empty again, so Clear and Run grey "
					 "themselves; the state is %s"),
				*SessionStateBits(State)),
			!State.bHasStructure);

		TestTrue(
			*FString::Printf(
				TEXT("CLEAR IS A COMMAND, NOT A MODE: the player must still be in Build mode, ready to "
					 "lay the next brick. The state is %s"),
				*SessionStateBits(State)),
			State.Mode == ESessionMode::Build);
	}

	Fixture.End();

	return true;
}

/**
 * The build level opens in Build mode with a plot open; other levels open in Destroy.
 *
 * On a scenario level, with nothing laid, the session structure falls back to the game mode's
 * built wall, so Run reaches it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionGameModeOpensBuildModeTest,
	"DestructionGame.World.Session.GameModeOpensBuildModeOnThePlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionGameModeOpensBuildModeTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionControllerTestSupport;

	// One: ?Scenario=build opens Build mode with a plot open.

	{
		FBrickTestWorld TestWorld;

		ADestructionGamePlayerController* Controller = nullptr;

		TestWorld.Wrapper.BeginPlayURL.AddOption(TEXT("Scenario=build"));

		TestWorld.Wrapper.BeforeBeginPlay = [&Controller](UWorld& World)
		{
			SessionSpawnPlayer(World, Controller);
		};

		if (!TestWorld.Begin(*this, ADestructionGameGameMode::StaticClass()))
		{
			TestWorld.End();
			return true;
		}

		if (Controller == nullptr)
		{
			AddError(TEXT("fixture: the build level must have a player controller in it"));
			TestWorld.End();
			return true;
		}

		UBuildModeComponent* const Build = Controller->GetBuildComponent();

		TestNotNull(TEXT("fixture: the controller must carry its build component"), Build);

		TestTrue(
			*FString::Printf(
				TEXT("THE BUILD LEVEL IS THE ONE LEVEL THAT OPENS IN BUILD MODE — it lays nothing, so "
					 "a session opened in Destroy would offer a player an empty plot and no way to put "
					 "anything on it. The state is %s"),
				*SessionStateBits(Controller->GetSessionToolbarState())),
			Controller->GetSessionToolbarState().Mode == ESessionMode::Build);

		if (Build != nullptr)
		{
			TestTrue(
				FString::Printf(
					TEXT("and a build must already be OPEN, so the first click lays a brick rather "
						 "than failing closed against an unknown structure; the component holds %d"),
					Build->GetStructureId()),
				Build->GetStructureId() != INDEX_NONE);
		}

		TestWorld.End();
	}

	// Two: ?Scenario=sandbox opens Destroy mode over its wall.

	{
		FBrickTestWorld TestWorld;

		ADestructionGamePlayerController* Controller = nullptr;

		TestWorld.Wrapper.BeginPlayURL.AddOption(TEXT("Scenario=sandbox"));

		TestWorld.Wrapper.BeforeBeginPlay = [&Controller](UWorld& World)
		{
			SessionSpawnPlayer(World, Controller);
		};

		if (!TestWorld.Begin(*this, ADestructionGameGameMode::StaticClass()))
		{
			TestWorld.End();
			return true;
		}

		ADestructionGameGameMode* const GameMode =
			TestWorld.World->GetAuthGameMode<ADestructionGameGameMode>();

		if (Controller == nullptr || GameMode == nullptr)
		{
			AddError(TEXT("fixture: the sandbox level must run the game's game mode with a player in "
						  "it"));

			TestWorld.End();
			return true;
		}

		TestTrue(
			*FString::Printf(
				TEXT("EVERY OTHER LEVEL OPENS IN DESTROY, unchanged: there is a wall in front of the "
					 "player and the click they will make is on it. The state is %s"),
				*SessionStateBits(Controller->GetSessionToolbarState())),
			Controller->GetSessionToolbarState().Mode == ESessionMode::Destroy);

		TestTrue(
			*FString::Printf(
				TEXT("fixture: the sandbox row must actually have built something for Run to act on; "
					 "GetBuiltStructureId is %d"),
				GameMode->GetBuiltStructureId()),
			GameMode->GetBuiltStructureId() != INDEX_NONE);

		TestEqual(
			FString::Printf(
				TEXT("AND RUN MUST REACH THE LEVEL'S OWN WALL: with nothing laid by the player, the "
					 "session's structure is the game mode's built %d; it names %d"),
				GameMode->GetBuiltStructureId(), Controller->GetSessionStructureId()),
			Controller->GetSessionStructureId(), GameMode->GetBuiltStructureId());

		TestWorld.End();
	}

	return true;
}

/**
 * A build whose only brick was deleted must not shadow the level's wall as the session structure.
 *
 * GetSessionStructureId once chose on NumPieces (one tombstone) while the flag read NumLivePieces
 * (zero), so Run greyed out over a 1,220-brick wall. Needs a real scenario level; the brick is laid
 * 30 m clear so the Destroy trace cannot hit the wall.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionStructureIsTheLiveOneTest,
	"DestructionGame.World.Session.SessionStructureIsTheLiveOne",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionStructureIsTheLiveOneTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionControllerTestSupport;

	FBrickTestWorld TestWorld;

	ADestructionGamePlayerController* Controller = nullptr;

	TestWorld.Wrapper.BeginPlayURL.AddOption(TEXT("Scenario=sandbox"));

	TestWorld.Wrapper.BeforeBeginPlay = [&Controller](UWorld& World)
	{
		SessionSpawnPlayer(World, Controller);
	};

	if (!TestWorld.Begin(*this, ADestructionGameGameMode::StaticClass()))
	{
		TestWorld.End();
		return true;
	}

	ADestructionGameGameMode* const GameMode =
		TestWorld.World->GetAuthGameMode<ADestructionGameGameMode>();

	if (Controller == nullptr || GameMode == nullptr)
	{
		AddError(TEXT("fixture: the sandbox level must run the game's game mode with a player in it"));
		TestWorld.End();
		return true;
	}

	UBuildModeComponent* const Build = Controller->GetBuildComponent();
	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	if (Build == nullptr)
	{
		AddError(TEXT("fixture: the controller must carry its build component"));
		TestWorld.End();
		return true;
	}

	// One: the level's own wall.

	const int32 BuiltStructureId = GameMode->GetBuiltStructureId();

	{
		const FStructureBinding* const Wall = Subsystem.Find(BuiltStructureId);

		TestTrue(
			*FString::Printf(
				TEXT("fixture: the sandbox row must have built a wall for any of this to mean "
					 "anything; GetBuiltStructureId is %d and the subsystem %s it"),
				BuiltStructureId, Wall != nullptr ? TEXT("holds") : TEXT("does not hold")),
			BuiltStructureId != INDEX_NONE && Wall != nullptr);

		if (Wall == nullptr)
		{
			TestWorld.End();
			return true;
		}

		TestTrue(
			FString::Printf(
				TEXT("fixture: and that wall must be STANDING — %d live piece(s) of %d laid"),
				Wall->GetStructure().NumLivePieces(), Wall->NumPieces()),
			Wall->GetStructure().NumLivePieces() > 0);
	}

	// Two: the player lays one brick well clear of it.

	if (!Controller->OnToolbarButton(EToolbarButtonId::ModeBuild))
	{
		AddError(TEXT("fixture: the Build tab must be clickable for any of this to run"));
		TestWorld.End();
		return true;
	}

	const int32 BuildStructureId = Build->GetStructureId();

	TestTrue(
		FString::Printf(
			TEXT("fixture: entering Build must open a plot, and it must be a DIFFERENT structure from "
				 "the level's wall %d; the component holds %d"),
			BuiltStructureId, BuildStructureId),
		BuildStructureId != INDEX_NONE && BuildStructureId != BuiltStructureId);

	{
		const bool bPlaced = Controller->PrimaryAlongRay(
			SessionPointerRayStart(SessionClearOfTheWallXCm),
			SessionPointerRayEnd(SessionClearOfTheWallXCm));

		TestTrue(
			FString::Printf(
				TEXT("fixture: the click must lay the player's one brick; it reported %d"),
				bPlaced ? 1 : 0),
			bPlaced);
	}

	FStructureBinding* Binding = Subsystem.Find(BuildStructureId);

	if (Binding == nullptr || Binding->NumPieces() != 1)
	{
		AddError(FString::Printf(
			TEXT("fixture: the build must hold exactly the one laid brick; it holds %d"),
			Binding != nullptr ? Binding->NumPieces() : INDEX_NONE));

		TestWorld.End();
		return true;
	}

	const FVector LaidCentreCm = Binding->GetBinding(0).Box.CentreCm;

	AddInfo(FString::Printf(
		TEXT("the player's brick is structure %d piece 0, laid at (%g, %g, %g); the level's wall is "
			 "structure %d"),
		BuildStructureId, LaidCentreCm.X, LaidCentreCm.Y, LaidCentreCm.Z, BuiltStructureId));

	// Three: and deletes it through the piece menu.

	if (!Controller->OnToolbarButton(EToolbarButtonId::ModeDestroy))
	{
		AddError(TEXT("fixture: the Destroy tab must be clickable"));
		TestWorld.End();
		return true;
	}

	{
		const FVector InspectStart(
			LaidCentreCm.X, LaidCentreCm.Y - SessionInspectReachCm, LaidCentreCm.Z);

		const FVector InspectEnd(
			LaidCentreCm.X, LaidCentreCm.Y + SessionInspectReachCm, LaidCentreCm.Z);

		Controller->PrimaryAlongRay(InspectStart, InspectEnd);

		const TArrayView<const FPieceMenuRow> Rows = Controller->GetShownPieceMenuRows();

		const int32 DeleteRow = SessionFindMenuRow(Rows, TEXT("Delete"));

		TestTrue(
			*FString::Printf(
				TEXT("fixture: the click must put the player's OWN brick's menu up, with a Delete row "
					 "against structure %d piece 0; it shows [%s]"),
				BuildStructureId, *SessionDescribeMenuRows(Rows)),
			DeleteRow != INDEX_NONE
				&& Rows.IsValidIndex(DeleteRow)
				&& Rows[DeleteRow].Ref.StructureId == BuildStructureId
				&& Rows[DeleteRow].Ref.PieceIndex == 0);

		if (DeleteRow == INDEX_NONE || Rows[DeleteRow].Ref.StructureId != BuildStructureId)
		{
			TestWorld.End();
			return true;
		}

		TestTrue(
			TEXT("fixture: choosing Delete must report that it committed"),
			Controller->ChoosePieceMenuRow(DeleteRow));
	}

	// Four: the plot is now a single tombstone.

	Binding = Subsystem.Find(BuildStructureId);

	if (Binding == nullptr)
	{
		AddError(TEXT("the build structure vanished under the delete"));
		TestWorld.End();
		return true;
	}

	TestEqual(
		FString::Printf(
			TEXT("fixture: RemovePiece tombstones rather than compacting, so the emptied build still "
				 "answers a piece count of 1; it answers %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 1);

	TestEqual(
		FString::Printf(
			TEXT("fixture: and NOTHING in it is live — which is the state the two readings disagree "
				 "about; %d are"),
			Binding->GetStructure().NumLivePieces()),
		Binding->GetStructure().NumLivePieces(), 0);

	// Five: so the session's structure is the level's wall again.

	TestEqual(
		FString::Printf(
			TEXT("AN EMPTIED BUILD MUST NOT SHADOW THE LEVEL'S WALL. The player has deleted the one "
				 "brick they laid, so there is nothing of theirs to command and the session must name "
				 "the wall standing in front of them, structure %d. It names %d — the build, which "
				 "holds one tombstone and no live piece, because the choice is made on NumPieces "
				 "while whether there is anything to run is decided on NumLivePieces"),
			BuiltStructureId, Controller->GetSessionStructureId()),
		Controller->GetSessionStructureId(), BuiltStructureId);

	// Six: and the strip offers Run against it.

	TestTrue(
		TEXT("fixture: the Destroy tab is always live, and clicking it is what refreshes the "
			 "precondition — the delete path itself never does"),
		Controller->OnToolbarButton(EToolbarButtonId::ModeDestroy));

	{
		const FSessionToolbarState& State = Controller->GetSessionToolbarState();

		TestTrue(
			*FString::Printf(
				TEXT("THERE IS A 1,220-BRICK WALL IN FRONT OF THE PLAYER, so the session must know it "
					 "has something to command. The state is %s"),
				*SessionStateBits(State)),
			State.bHasStructure);

		TestTrue(
			*FString::Printf(
				TEXT("AND RUN STRUCTURE MUST BE DRAWN LIVE. One deleted brick greying the only command "
					 "on the Destroy strip, over a wall that is plainly standing there, is a dead "
					 "button with nothing on screen saying why. The state is %s"),
				*SessionStateBits(State)),
			SessionButtonIsEnabled(State, EToolbarButtonId::RunStructure));
	}

	TestWorld.End();

	return true;
}

/**
 * Deleting the last piece clears bHasStructure immediately, without a toolbar click.
 *
 * The read is cold: no toolbar click between delete and read, since OnToolbarButton would refresh
 * the flag itself. A bare world with no level wall, so the session genuinely names no structure.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionDeleteRefreshesTheFlagTest,
	"DestructionGame.World.Session.DeleteRefreshesTheSessionFlag",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionDeleteRefreshesTheFlagTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionControllerTestSupport;

	FSessionFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;
	UBuildModeComponent& Build = *Fixture.Build;
	UDestructionStructureSubsystem& Subsystem = *Fixture.TestWorld.Subsystem;

	if (!Controller.OnToolbarButton(EToolbarButtonId::ModeBuild))
	{
		AddError(TEXT("fixture: the Build tab must be clickable for any of this to run"));
		Fixture.End();
		return true;
	}

	const int32 BuildStructureId = Build.GetStructureId();

	// One: one brick, and the session knows it has a structure.

	{
		const bool bPlaced = Controller.PrimaryAlongRay(
			SessionPointerRayStart(0.0), SessionPointerRayEnd(0.0));

		TestTrue(
			FString::Printf(
				TEXT("fixture: the click must lay the player's one brick; it reported %d"),
				bPlaced ? 1 : 0),
			bPlaced);
	}

	FStructureBinding* Binding = Subsystem.Find(BuildStructureId);

	if (Binding == nullptr || Binding->NumPieces() != 1)
	{
		AddError(FString::Printf(
			TEXT("fixture: the build must hold exactly the one laid brick; it holds %d"),
			Binding != nullptr ? Binding->NumPieces() : INDEX_NONE));

		Fixture.End();
		return true;
	}

	const FVector LaidCentreCm = Binding->GetBinding(0).Box.CentreCm;

	if (!Controller.OnToolbarButton(EToolbarButtonId::ModeDestroy))
	{
		AddError(TEXT("fixture: the Destroy tab must be clickable"));
		Fixture.End();
		return true;
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: with one brick laid and the mode switched, the session must already KNOW "
				 "it has a structure — otherwise 'false afterwards' says nothing. The state is %s"),
			*SessionStateBits(Controller.GetSessionToolbarState())),
		Controller.GetSessionToolbarState().bHasStructure);

	// Two: the player deletes it.

	{
		const FVector InspectStart(
			LaidCentreCm.X, LaidCentreCm.Y - SessionInspectReachCm, LaidCentreCm.Z);

		const FVector InspectEnd(
			LaidCentreCm.X, LaidCentreCm.Y + SessionInspectReachCm, LaidCentreCm.Z);

		Controller.PrimaryAlongRay(InspectStart, InspectEnd);

		const TArrayView<const FPieceMenuRow> Rows = Controller.GetShownPieceMenuRows();

		const int32 DeleteRow = SessionFindMenuRow(Rows, TEXT("Delete"));

		TestTrue(
			*FString::Printf(
				TEXT("fixture: the click must put the laid brick's menu up with a Delete row against "
					 "structure %d piece 0; it shows [%s]"),
				BuildStructureId, *SessionDescribeMenuRows(Rows)),
			DeleteRow != INDEX_NONE
				&& Rows.IsValidIndex(DeleteRow)
				&& Rows[DeleteRow].Ref.StructureId == BuildStructureId
				&& Rows[DeleteRow].Ref.PieceIndex == 0);

		if (DeleteRow == INDEX_NONE || Rows[DeleteRow].Ref.StructureId != BuildStructureId)
		{
			Fixture.End();
			return true;
		}

		TestTrue(
			TEXT("fixture: choosing Delete must report that it committed"),
			Controller.ChoosePieceMenuRow(DeleteRow));
	}

	// No toolbar clicks from here on: any click, even a refused one, refreshes the flag.
	Binding = Subsystem.Find(BuildStructureId);

	if (Binding == nullptr)
	{
		AddError(TEXT("the build structure vanished under the delete"));
		Fixture.End();
		return true;
	}

	TestEqual(
		FString::Printf(
			TEXT("fixture: RemovePiece tombstones rather than compacting, so the emptied build still "
				 "answers a piece count of 1; it answers %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 1);

	TestEqual(
		FString::Printf(
			TEXT("fixture: and nothing in it is live — the plot is empty; %d are"),
			Binding->GetStructure().NumLivePieces()),
		Binding->GetStructure().NumLivePieces(), 0);

	TestEqual(
		FString::Printf(
			TEXT("fixture: and this bare world laid no wall of its own, so there is genuinely nothing "
				 "for the session to name; it names %d"),
			Controller.GetSessionStructureId()),
		Controller.GetSessionStructureId(), static_cast<int32>(INDEX_NONE));

	// Three: the cold read.

	{
		const FSessionToolbarState& State = Controller.GetSessionToolbarState();

		TestTrue(
			*FString::Printf(
				TEXT("THE DELETE MUST REFRESH THE SESSION'S ONE PRECONDITION. The player's only brick "
					 "is gone and nothing else stands, so the state must say so WITHOUT waiting for "
					 "the next toolbar click — the strip on screen is drawn from this, and an unrefreshed "
					 "flag goes on offering Run and Clear over an empty plot until something else is "
					 "pressed. The state is %s"),
				*SessionStateBits(State)),
			!State.bHasStructure);

		TestTrue(
			*FString::Printf(
				TEXT("AND THE STRIP MUST THEREFORE GREY Run structure. A lit command over a plot with "
					 "nothing on it is a button that solves an empty graph and reports success, which "
					 "is indistinguishable from the game having missed the click. The state is %s"),
				*SessionStateBits(State)),
			!SessionButtonIsEnabled(State, EToolbarButtonId::RunStructure));
	}

	Fixture.End();

	return true;
}

/**
 * CR-2b: the rotate chip swaps the ghost's X and Y half extents, and a rotated brick beside a laid
 * one snaps to the corner return.
 *
 * FPieceBox is axis-aligned, so the swapped extent is the rotation. The build plane must not move.
 * The corner return is flush with the seed's -Y face with one full GeneralPurposeMortar joint; the
 * cursor at Y = 5.0 is 0.625 cm from it against 10.625 cm for the nearest other pose.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionRotateSwapsTheGhostFootprintTest,
	"DestructionGame.World.Session.RotateSwapsTheGhostFootprint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionRotateSwapsTheGhostFootprintTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionControllerTestSupport;

	// Footprints spelled out, not derived from the palette whose swap is under test.
	const FVector BrickUprightHalfCm(10.75, 5.125, 3.25);
	const FVector BrickRotatedHalfCm(5.125, 10.75, 3.25);
	const FVector PlateRotatedHalfCm(5.125, 33.75, 5.0);

	/*
	 * Corner return off the seed at the origin: X = 10.75 + 1.0 + 5.125, Y = -5.125 + 10.75. The
	 * quoin is the seed's 10.25 x 6.5 cm end face.
	 */
	const FVector CornerReturnCentreCm(16.875, 5.625, 3.25);
	constexpr double CornerCursorXCm = 16.875;
	constexpr double CornerCursorYCm = 5.0;
	constexpr double QuoinAreaSqCm = 66.625;

	// A double; KINDA_SMALL_NUMBER is a float and makes TestEqual's overload ambiguous.
	constexpr double SessionPlaneToleranceCm = 1.0e-6;

	FSessionFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;
	UBuildModeComponent& Build = *Fixture.Build;
	UDestructionStructureSubsystem& Subsystem = *Fixture.TestWorld.Subsystem;

	if (!Controller.OnToolbarButton(EToolbarButtonId::ModeBuild))
	{
		AddError(TEXT("fixture: the Build tab must be clickable for any of this to run"));
		Fixture.End();
		return true;
	}

	const int32 BuildStructureId = Build.GetStructureId();

	// One: a session opens with an upright brick.

	{
		TestFalse(
			*FString::Printf(
				TEXT("a fresh session must not be rotated; the state is %s"),
				*SessionStateBits(Controller.GetSessionToolbarState())),
			Controller.GetSessionToolbarState().bRotated);

		TestFalse(
			TEXT("and the component must agree — the copy the world is driven from is the one that "
				 "decides what lands"),
			Build.IsRotated());

		TestTrue(
			*FString::Printf(
				TEXT("the brick's upright half extent must be (10.75, 5.125, 3.25) — the 21.5 x 10.25 x "
					 "6.5 unit halved, lying along X; it is (%g, %g, %g)"),
				Build.CurrentExtentCm.X, Build.CurrentExtentCm.Y, Build.CurrentExtentCm.Z),
			Build.CurrentExtentCm.Equals(BrickUprightHalfCm, KINDA_SMALL_NUMBER));

		TestEqual(
			FString::Printf(
				TEXT("and the course-0 build plane for a brick is its own half height, 3.25; it is %g"),
				Build.BuildPlaneZCm),
			Build.BuildPlaneZCm, SessionBrickPlaneCourse0Cm, SessionPlaneToleranceCm);
	}

	// Two: the click swaps X and Y and leaves Z.

	{
		TestTrue(
			TEXT("THE ROTATE CHIP MUST BE CLICKABLE IN EVERY BUILD STATE — rotation has no "
				 "precondition, it describes the next placement"),
			Controller.OnToolbarButton(EToolbarButtonId::RotatePiece));

		TestTrue(
			*FString::Printf(
				TEXT("the session must record the rotation, because the strip's lit chip is drawn from "
					 "it; the state is %s"),
				*SessionStateBits(Controller.GetSessionToolbarState())),
			Controller.GetSessionToolbarState().bRotated);

		TestTrue(
			TEXT("AND THE COMPONENT MUST BE PUSHED. The state is the presenter's record and the "
				 "component is what the world does; a click that moved only the first lights a chip "
				 "over a ghost that has not turned"),
			Build.IsRotated());

		TestTrue(
			*FString::Printf(
				TEXT("THE ROTATED BRICK'S HALF EXTENT IS THE UPRIGHT ONE WITH X AND Y SWAPPED — "
					 "(5.125, 10.75, 3.25), a header lying along Y. There is no other representation "
					 "of 'rotated' downstream: FPieceBox is axis-aligned, so this IS the rotation. It "
					 "is (%g, %g, %g)"),
				Build.CurrentExtentCm.X, Build.CurrentExtentCm.Y, Build.CurrentExtentCm.Z),
			Build.CurrentExtentCm.Equals(BrickRotatedHalfCm, KINDA_SMALL_NUMBER));

		TestEqual(
			FString::Printf(
				TEXT("AND THE BUILD PLANE MUST NOT MOVE: Z is untouched by a rotation about it, so the "
					 "course-0 plane is still 3.25. It is %g"),
				Build.BuildPlaneZCm),
			Build.BuildPlaneZCm, SessionBrickPlaneCourse0Cm, SessionPlaneToleranceCm);
	}

	/*
	 * Three: choosing a piece while rotated re-derives the swapped extent. The plate's half height
	 * (5.0) is not a swapped number, so a wrong-axis plane shows as 5.125.
	 */
	{
		TestTrue(
			TEXT("a piece chip is always live in Build mode"),
			Controller.OnToolbarButton(EToolbarButtonId::PieceTimberPlate));

		TestTrue(
			*FString::Printf(
				TEXT("THE PLATE MUST BE DERIVED ROTATED TOO — (5.125, 33.75, 5.0), the demo's 67.5 cm "
					 "board turned to run along Y. SetPieceKind re-derives from the palette, so it has "
					 "to honour a rotation chosen before it. It is (%g, %g, %g)"),
				Build.CurrentExtentCm.X, Build.CurrentExtentCm.Y, Build.CurrentExtentCm.Z),
			Build.CurrentExtentCm.Equals(PlateRotatedHalfCm, KINDA_SMALL_NUMBER));

		TestEqual(
			FString::Printf(
				TEXT("and the plate's course-0 plane is ITS OWN half height, 5.0 — never the 5.125 a "
					 "plane taken off the swapped X would give. It is %g"),
				Build.BuildPlaneZCm),
			Build.BuildPlaneZCm, SessionPlatePlaneCourse0Cm, SessionPlaneToleranceCm);

		TestTrue(TEXT("back to the brick"), Controller.OnToolbarButton(EToolbarButtonId::PieceBrick));
	}

	// Four: the same chip turns it back.

	{
		TestTrue(
			TEXT("the second click on the chip must land too"),
			Controller.OnToolbarButton(EToolbarButtonId::RotatePiece));

		TestFalse(
			*FString::Printf(
				TEXT("A SETTING THE PLAYER CANNOT UNSET IS NOT A SETTING: the session must read "
					 "upright again. It is %s"),
				*SessionStateBits(Controller.GetSessionToolbarState())),
			Controller.GetSessionToolbarState().bRotated);

		TestFalse(TEXT("and so must the component"), Build.IsRotated());

		TestTrue(
			*FString::Printf(
				TEXT("and the brick's footprint is back to the upright (10.75, 5.125, 3.25); it is "
					 "(%g, %g, %g)"),
				Build.CurrentExtentCm.X, Build.CurrentExtentCm.Y, Build.CurrentExtentCm.Z),
			Build.CurrentExtentCm.Equals(BrickUprightHalfCm, KINDA_SMALL_NUMBER));
	}

	// Five: an upright seed brick on the earth.

	{
		const bool bPlaced = Controller.PrimaryAlongRay(
			SessionPointerRayStart(0.0), SessionPointerRayEnd(0.0));

		TestTrue(
			FString::Printf(
				TEXT("fixture: the seed brick must land at the origin; the click reported %d"),
				bPlaced ? 1 : 0),
			bPlaced);
	}

	// Six: rotated, the ghost beside it previews a corner return.

	TestTrue(
		TEXT("rotate again, to lay the return"),
		Controller.OnToolbarButton(EToolbarButtonId::RotatePiece));

	const FVector CornerRayStart(CornerCursorXCm, CornerCursorYCm, SessionRayStartZCm);
	const FVector CornerRayEnd(CornerCursorXCm, CornerCursorYCm, SessionRayEndZCm);

	// The player's own seam first: a moving cursor shows the ghost.
	Controller.PointerAlongRay(CornerRayStart, CornerRayEnd);

	if (AActor* const Ghost = Build.GetGhostActor())
	{
		TestFalse(
			TEXT("pointing beside the seed with a rotated piece must SHOW the ghost — the player has "
				 "to see the return before they commit to it"),
			Ghost->IsHidden());
	}
	else
	{
		AddError(TEXT("pointing in Build mode must reach the component's preview, which spawns its "
					  "ghost"));
	}

	/*
	 * Read the pose via UpdatePreviewFromRay, the call PointerAlongRay makes; the ghost's transform
	 * pivots on a corner, not the centre.
	 */
	{
		const FBuildPreview Preview =
			Build.UpdatePreviewFromRay(CornerRayStart, CornerRayEnd - CornerRayStart);

		AddInfo(FString::Printf(
			TEXT("the rotated preview beside the seed is kind %d at (%.4f, %.4f, %.4f) with %d "
				 "joint(s), valid %d, grounded %d"),
			static_cast<int32>(Preview.Kind), Preview.CentreCm.X, Preview.CentreCm.Y,
			Preview.CentreCm.Z, Preview.JointCount, Preview.bValid ? 1 : 0,
			Preview.bGrounded ? 1 : 0));

		TestEqual(
			FString::Printf(
				TEXT("A ROTATED BRICK BESIDE AN X-LONG ONE MUST PREVIEW A **CORNER RETURN** (%d), the "
					 "pose CR-2a added and the one no player could reach until this chip existed; it "
					 "previews %d"),
				static_cast<int32>(BuildMode::ESnapKind::BrickCornerReturn),
				static_cast<int32>(Preview.Kind)),
			static_cast<int32>(Preview.Kind),
			static_cast<int32>(BuildMode::ESnapKind::BrickCornerReturn));

		TestTrue(
			*FString::Printf(
				TEXT("and at the pose that TURNS THE CORNER — (16.875, 5.625, 3.25), finishing flush "
					 "with the seed's -Y face. Four return poses exist off one neighbour and only this "
					 "one builds an L; it previews (%g, %g, %g)"),
				Preview.CentreCm.X, Preview.CentreCm.Y, Preview.CentreCm.Z),
			Preview.CentreCm.Equals(CornerReturnCentreCm, KINDA_SMALL_NUMBER));

		TestEqual(
			FString::Printf(
				TEXT("forming exactly ONE joint — the quoin onto the seed's end face; it previews %d"),
				Preview.JointCount),
			Preview.JointCount, 1);
	}

	// Seven: the click commits it, and the quoin is full mortar.

	{
		const bool bPlaced = Controller.PrimaryAlongRay(CornerRayStart, CornerRayEnd);

		TestTrue(
			FString::Printf(
				TEXT("the click on the return must land; it reported %d"), bPlaced ? 1 : 0),
			bPlaced);

		FStructureBinding* const Binding = Subsystem.Find(BuildStructureId);

		if (Binding == nullptr || Binding->NumPieces() != 2)
		{
			AddError(FString::Printf(
				TEXT("the build must hold the seed and the return, 2 pieces; it holds %d"),
				Binding != nullptr ? Binding->NumPieces() : INDEX_NONE));

			Fixture.End();
			return true;
		}

		const FVector CentreCm = Binding->GetBinding(1).Box.CentreCm;
		const FVector ExtentCm = Binding->GetBinding(1).Box.ExtentCm;

		TestTrue(
			FString::Printf(
				TEXT("the committed piece must be at the pose the ghost showed, (16.875, 5.625, 3.25); "
					 "it is (%g, %g, %g)"),
				CentreCm.X, CentreCm.Y, CentreCm.Z),
			CentreCm.Equals(CornerReturnCentreCm, KINDA_SMALL_NUMBER));

		TestTrue(
			FString::Printf(
				TEXT("AND IT MUST BE THE TURNED BOX THAT LANDED, half (5.125, 10.75, 3.25) — a "
					 "committed piece carrying the upright footprint would stand across the corner it "
					 "was meant to turn. It is (%g, %g, %g)"),
				ExtentCm.X, ExtentCm.Y, ExtentCm.Z),
			ExtentCm.Equals(BrickRotatedHalfCm, KINDA_SMALL_NUMBER));

		TestEqual(
			FString::Printf(
				TEXT("and exactly one connection joins the two; the build holds %d"),
				Binding->GetStructure().NumConnections()),
			Binding->GetStructure().NumConnections(), 1);

		if (Binding->GetStructure().NumConnections() == 1)
		{
			const FConnection& Quoin = Binding->GetStructure().GetConnection(0);
			const FConnectionStrength& Got = Quoin.Strength;
			const FConnectionStrength& Want = DestructionProfiles::GeneralPurposeMortar;

			// All five fields: mortar and the weak perpend differ only on cohesion and tension.
			const bool bIsFullMortar =
				Got.CompressiveStrengthMPa == Want.CompressiveStrengthMPa
				&& Got.ShearCohesionMPa == Want.ShearCohesionMPa
				&& Got.TensileStrengthMPa == Want.TensileStrengthMPa
				&& Got.FrictionCoefficient == Want.FrictionCoefficient
				&& Got.MaxShearStrengthMPa == Want.MaxShearStrengthMPa;

			AddInfo(FString::Printf(
				TEXT("the quoin (%d-%d) is {c %g, coh %g, t %g, mu %g, cap %g} over %g cm2, normal "
					 "(%g, %g, %g)"),
				Quoin.PieceA, Quoin.PieceB, Got.CompressiveStrengthMPa, Got.ShearCohesionMPa,
				Got.TensileStrengthMPa, Got.FrictionCoefficient, Got.MaxShearStrengthMPa,
				Quoin.InterfaceAreaSqCm, Quoin.InterfaceNormal.X, Quoin.InterfaceNormal.Y,
				Quoin.InterfaceNormal.Z));

			TestTrue(
				*FString::Printf(
					TEXT("THE PLAYER'S CORNER MUST BE A BONDED QUOIN — full GeneralPurposeMortar over "
						 "a vertical face, which is the whole of the 2026-09-15 ruling. The "
						 "pre-CR-2a inference called this contact a weak perpend, and the two differ "
						 "on cohesion and tension alone. It reads {coh %g, t %g}"),
					Got.ShearCohesionMPa, Got.TensileStrengthMPa),
				bIsFullMortar);

			TestEqual(
				TEXT("over the seed's end face, 10.25 cm of width by 6.5 cm of course = 66.625 cm2"),
				Quoin.InterfaceAreaSqCm, QuoinAreaSqCm, 1.0e-6);

			// A horizontal normal, or full mortar would just be an ordinary bed.
			TestEqual(
				TEXT("across a HORIZONTAL normal on the X axis (|X| == 1) — the seed's end face, not "
					 "a bed"),
				FMath::Abs(Quoin.InterfaceNormal.X), 1.0, SessionPlaneToleranceCm);
		}
	}

	Fixture.End();

	return true;
}

/**
 * CR-2b (xiii): a corner the player lays is judged by the LP, not silently demoted to the router.
 *
 * BeginBuild must flag the build 3D, or the bridge refuses the whole problem on the first Y-normal
 * joint and BreakByEquilibrium silently declines to the router, which stands the wall anyway
 * (DESIGN §5). Checked on the empty build (the flag is stated, never inferred; E3 ruling,
 * Structure.h) and after Run (every posed joint has a readout; absent means the gate declined).
 *
 * Six pieces, because the bridge skips grounded-grounded joints: the course-1 head 5-4 is the one
 * posed Y-normal joint. Poses follow CornerWallStands.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionCornerBuildIsJudgedByTheLPTest,
	"DestructionGame.World.Session.CornerBuildIsJudgedByTheLP",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionCornerBuildIsJudgedByTheLPTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionControllerTestSupport;

	// The cursor for each of the six clicks, in order.
	struct FCornerLayStep
	{
		double CursorXCm;
		double CursorYCm;
		bool bRotated;
		int32 Course;
	};

	const FCornerLayStep CornerLaySteps[] = {
		{  0.000,  0.000, false, 0 },   /* the X leg's one stretcher, on the earth */
		{ 16.875,  5.000, true,  0 },   /* the return — the quoin, across the seed's end face */
		{ 16.875, 28.000, true,  0 },   /* the Y leg, same course */
		{ 16.875, 50.500, true,  0 },
		{ 16.875, 16.875, true,  1 },   /* course 1, staggered over the two below */
		{ 16.875, 39.375, true,  1 },
	};

	constexpr int32 CornerExpectedPieces = 6;
	constexpr int32 CornerExpectedConnections = 8;

	/*
	 * Skipped (earth-to-earth): the quoin 1-0 and course-0 heads 2-1, 3-2. Posed: the four beds and
	 * the course-1 head 5-4.
	 */
	constexpr int32 CornerPosedJoints = 5;
	constexpr int32 CornerSkippedJoints = 3;

	/** Pieces 0..3 are on the earth; 4 and 5 reach it through beds. */
	constexpr int32 CornerLastGroundedPiece = 3;

	constexpr double SessionNormalToleranceCm = 1.0e-6;

	FSessionFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;
	UBuildModeComponent& Build = *Fixture.Build;
	UDestructionStructureSubsystem& Subsystem = *Fixture.TestWorld.Subsystem;

	if (!Controller.OnToolbarButton(EToolbarButtonId::ModeBuild))
	{
		AddError(TEXT("fixture: the Build tab must be clickable for any of this to run"));
		Fixture.End();
		return true;
	}

	const int32 BuildStructureId = Build.GetStructureId();

	// One: the flag is set when the empty build opens.

	{
		const FStructureBinding* const Fresh = Subsystem.Find(BuildStructureId);

		if (Fresh == nullptr)
		{
			AddError(FString::Printf(
				TEXT("fixture: entering Build mode must open a binding; structure %d names nothing"),
				BuildStructureId));

			Fixture.End();
			return true;
		}

		TestTrue(
			*FString::Printf(
				TEXT("THE EMPTY BUILD MUST ALREADY BE FLAGGED 3D. BeginBuild has to STATE the intent, "
					 "never infer it when the first Y-normal joint lands: the E3 ruling "
					 "(Structure.h ~490) is that 3D is stated and a 2D structure which has accidentally "
					 "acquired an out-of-plane joint stays loudly refused — and an inferred flag would "
					 "put a cliff mid-build, where the brick that lands moves the authority deciding "
					 "whether the wall stands. Structure %d with %d pieces reads IsThreeDimensional() "
					 "== false"),
				BuildStructureId, Fresh->NumPieces()),
			Fresh->GetStructure().IsThreeDimensional());
	}

	// Two: lay the L through the player's clicks.

	{
		int32 Course = 0;
		bool bRotated = false;

		for (int32 Step = 0; Step < UE_ARRAY_COUNT(CornerLaySteps); ++Step)
		{
			const FCornerLayStep& Lay = CornerLaySteps[Step];

			if (Lay.bRotated != bRotated)
			{
				Controller.OnToolbarButton(EToolbarButtonId::RotatePiece);
				bRotated = Lay.bRotated;
			}

			while (Course < Lay.Course)
			{
				Controller.OnToolbarButton(EToolbarButtonId::CourseUp);
				++Course;
			}

			const bool bPlaced = Controller.PrimaryAlongRay(
				SessionPointerRayStartAt(Lay.CursorXCm, Lay.CursorYCm),
				SessionPointerRayEndAt(Lay.CursorXCm, Lay.CursorYCm));

			if (!bPlaced)
			{
				AddError(FString::Printf(
					TEXT("fixture: click %d at (%g, %g), course %d, rotated %d must lay a brick; it "
						 "reported nothing placed"),
					Step, Lay.CursorXCm, Lay.CursorYCm, Lay.Course, Lay.bRotated ? 1 : 0));
			}
		}

		TestTrue(
			*FString::Printf(
				TEXT("fixture: the session must still be rotated and on course 1 after the last click; "
					 "the state is %s"),
				*SessionStateBits(Controller.GetSessionToolbarState())),
			Controller.GetSessionToolbarState().bRotated
				&& Controller.GetSessionToolbarState().Course == 1);
	}

	FStructureBinding* Binding = Subsystem.Find(BuildStructureId);

	if (Binding == nullptr
		|| Binding->NumPieces() != CornerExpectedPieces
		|| Binding->GetStructure().NumConnections() != CornerExpectedConnections)
	{
		AddError(FString::Printf(
			TEXT("fixture: the six clicks must give a %d-piece, %d-connection L; the build holds %d "
				 "pieces and %d connections"),
			CornerExpectedPieces, CornerExpectedConnections,
			Binding != nullptr ? Binding->NumPieces() : INDEX_NONE,
			Binding != nullptr ? Binding->GetStructure().NumConnections() : INDEX_NONE));

		Fixture.End();
		return true;
	}

	// Three: fixture guards, a real corner with one posed Y-normal joint.

	{
		const FStructure& Structure = Binding->GetStructure();

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			AddInfo(SessionDescribeJoint(Structure, Joint));
		}

		for (int32 Piece = 0; Piece < Structure.NumPieces(); ++Piece)
		{
			const bool bWantGrounded = Piece <= CornerLastGroundedPiece;

			TestEqual(
				FString::Printf(
					TEXT("fixture: piece %d must be %s — the two course-1 bricks reach the earth only "
						 "through their beds, which is what makes their joints POSED rather than "
						 "skipped as earth-to-earth"),
					Piece, bWantGrounded ? TEXT("grounded") : TEXT("off the earth")),
				Structure.GetPiece(Piece).bIsGrounded, bWantGrounded);
		}

		int32 Quoins = 0;
		int32 PosedOutOfPlane = 0;
		int32 Posed = 0;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			const FConnection& Connection = Structure.GetConnection(Joint);
			const bool bIsPosed = SessionJointIsPosedByTheLP(Structure, Joint);

			Posed += bIsPosed ? 1 : 0;

			if (FMath::Abs(FMath::Abs(Connection.InterfaceNormal.X) - 1.0) < SessionNormalToleranceCm)
			{
				++Quoins;
			}

			if (bIsPosed
				&& FMath::Abs(FMath::Abs(Connection.InterfaceNormal.Y) - 1.0) < SessionNormalToleranceCm)
			{
				++PosedOutOfPlane;
			}
		}

		TestEqual(
			FString::Printf(
				TEXT("fixture: exactly one joint across the X axis — the QUOIN, the seed's end face "
					 "that the rotated return abuts. The build holds %d"),
				Quoins),
			Quoins, 1);

		TestEqual(
			*FString::Printf(
				TEXT("FIXTURE, AND THE WHOLE POINT OF THE SIX PIECES: exactly one POSED joint across "
					 "the Y axis — the course-1 head 5-4, between two pieces neither of which is "
					 "grounded. This is the out-of-plane normal a 2D bridge refuses; with every "
					 "Y-normal joint earth-to-earth the bridge would skip them all, never reach the "
					 "refusal, and this test would be green for the wrong reason. It holds %d"),
				PosedOutOfPlane),
			PosedOutOfPlane, 1);

		TestEqual(
			FString::Printf(
				TEXT("fixture: %d of the %d joints are posed (the rest join two grounded pieces and are "
					 "skipped by the bridge); %d are posed"),
				CornerPosedJoints, CornerExpectedConnections, Posed),
			Posed, CornerPosedJoints);

		TestEqual(
			FString::Printf(
				TEXT("fixture: and %d are the earth-to-earth ones — the quoin and the two course-0 "
					 "heads; %d are"),
				CornerSkippedJoints, Structure.NumConnections() - Posed),
			Structure.NumConnections() - Posed, CornerSkippedJoints);
	}

	// Four: the mechanism, the finished corner build is 3D.

	TestTrue(
		*FString::Printf(
			TEXT("THE PLAYER'S CORNER BUILD MUST BE FLAGGED 3D. It carries a Y-normal head joint, and "
				 "`BuildRigidBlockProblem` refuses the WHOLE problem on the first out-of-plane normal "
				 "it meets unless the structure states it is 3D — 'a 2D X-Z oracle must refuse rather "
				 "than project'. Nothing in the session sets it: only the scenario builders do, and "
				 "AdoptLayout carries THEIR flag across. Structure %d reads IsThreeDimensional() == "
				 "false with %d pieces and %d connections"),
			BuildStructureId, Binding->NumPieces(), Binding->GetStructure().NumConnections()),
		Binding->GetStructure().IsThreeDimensional());

	// Five: the outcome, Run is answered by the LP, not the router.

	TestTrue(
		TEXT("the Destroy tab is always live"),
		Controller.OnToolbarButton(EToolbarButtonId::ModeDestroy));

	TestEqual(
		FString::Printf(
			TEXT("fixture: the session must run the PLAYER'S build, structure %d; it names %d"),
			BuildStructureId, Controller.GetSessionStructureId()),
		Controller.GetSessionStructureId(), BuildStructureId);

	TestTrue(
		TEXT("fixture: clicking Run on a live build must report that it landed"),
		Controller.OnToolbarButton(EToolbarButtonId::RunStructure));

	Binding = Subsystem.Find(BuildStructureId);

	if (Binding == nullptr)
	{
		AddError(TEXT("the build must survive its own Run"));
		Fixture.End();
		return true;
	}

	{
		const FStructure& Structure = Binding->GetStructure();

		AddInfo(FString::Printf(
			TEXT("after Run: %d piece(s) released, %d min-violation readout solve(s)"),
			SessionCountReleased(*Binding), Structure.GetMinViolationReadoutSolveCount()));

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			AddInfo(SessionDescribeJoint(Structure, Joint));
		}

		TestEqual(
			FString::Printf(
				TEXT("A BONDED L MUST STILL BE STANDING AFTER RUN — nothing may be released. This is "
					 "the weak half of the claim and it passes today through the router; it is here so "
					 "a fix that reaches the LP cannot pay for it by dropping the player's wall. %d "
					 "piece(s) went"),
				SessionCountReleased(*Binding)),
			SessionCountReleased(*Binding), 0);

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			if (!SessionJointIsPosedByTheLP(Structure, Joint))
			{
				continue;
			}

			TestTrue(
				*FString::Printf(
					TEXT("THE LP MUST HAVE ANSWERED THIS BUILD: every posed joint carries a "
						 "min-violation readout after a below-cap settle, and this one does not. An "
						 "ABSENT readout is the gate having DECLINED — the cache is cleared at the top "
						 "of every pass and refilled only by an arm that reached a verdict — and the "
						 "decline is silent: the 2D bridge refused the whole problem on the "
						 "out-of-plane (Y) normal of the course-1 head, so BreakByCapacitySweep, not "
						 "the LP, decided whether this corner stands. %s"),
					*SessionDescribeJoint(Structure, Joint)),
				Structure.GetConnectionReadout(Joint).bPresent);
		}
	}

	Fixture.End();

	return true;
}

/**
 * CR-2b (xiii) control: a straight build is flagged 3D too, and still reads the LP.
 *
 * With no rotated piece or Y-normal joint, only an unconditional flag at BeginBuild passes (E3
 * ruling). The readout half guards against the fix taking the LP off planar walls. Three bricks
 * in running bond; the two beds are the posed joints.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionStraightBuildIsJudgedByTheLPTest,
	"DestructionGame.World.Session.StraightBuildIsJudgedByTheLP",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionStraightBuildIsJudgedByTheLPTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionControllerTestSupport;

	constexpr int32 StraightExpectedPieces = 3;
	constexpr int32 StraightExpectedConnections = 3;
	constexpr int32 StraightPosedJoints = 2;

	// Course 1's running-bond stagger.
	constexpr double StraightCourse1XCm = 11.25;

	FSessionFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;
	UBuildModeComponent& Build = *Fixture.Build;
	UDestructionStructureSubsystem& Subsystem = *Fixture.TestWorld.Subsystem;

	if (!Controller.OnToolbarButton(EToolbarButtonId::ModeBuild))
	{
		AddError(TEXT("fixture: the Build tab must be clickable for any of this to run"));
		Fixture.End();
		return true;
	}

	const int32 BuildStructureId = Build.GetStructureId();

	// One: the flag at the door, with no rotation anywhere in this test.

	{
		const FStructureBinding* const Fresh = Subsystem.Find(BuildStructureId);

		if (Fresh == nullptr)
		{
			AddError(FString::Printf(
				TEXT("fixture: entering Build mode must open a binding; structure %d names nothing"),
				BuildStructureId));

			Fixture.End();
			return true;
		}

		TestTrue(
			*FString::Printf(
				TEXT("EVERY BUILD IS OPENED 3D, INCLUDING THIS ONE. The Rotate chip is never clicked in "
					 "this test and no joint here has a Y normal, so a flag set on a rotation — or "
					 "inferred from an out-of-plane joint — leaves this build 2D and lets the fix be "
					 "rotation-conditional, which is a cliff rather than a rule. Structure %d reads "
					 "IsThreeDimensional() == false"),
				BuildStructureId),
			Fresh->GetStructure().IsThreeDimensional());
	}

	// Two: two courses of running bond.

	Controller.PrimaryAlongRay(SessionPointerRayStart(0.0), SessionPointerRayEnd(0.0));

	Controller.PrimaryAlongRay(
		SessionPointerRayStart(SessionSecondCursorXCm), SessionPointerRayEnd(SessionSecondCursorXCm));

	Controller.OnToolbarButton(EToolbarButtonId::CourseUp);

	Controller.PrimaryAlongRay(
		SessionPointerRayStart(StraightCourse1XCm), SessionPointerRayEnd(StraightCourse1XCm));

	FStructureBinding* Binding = Subsystem.Find(BuildStructureId);

	if (Binding == nullptr
		|| Binding->NumPieces() != StraightExpectedPieces
		|| Binding->GetStructure().NumConnections() != StraightExpectedConnections)
	{
		AddError(FString::Printf(
			TEXT("fixture: the three clicks must give a %d-piece, %d-connection wall; the build holds "
				 "%d pieces and %d connections"),
			StraightExpectedPieces, StraightExpectedConnections,
			Binding != nullptr ? Binding->NumPieces() : INDEX_NONE,
			Binding != nullptr ? Binding->GetStructure().NumConnections() : INDEX_NONE));

		Fixture.End();
		return true;
	}

	{
		const FStructure& Structure = Binding->GetStructure();

		int32 Posed = 0;
		int32 OutOfPlane = 0;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			AddInfo(SessionDescribeJoint(Structure, Joint));

			Posed += SessionJointIsPosedByTheLP(Structure, Joint) ? 1 : 0;
			OutOfPlane += FMath::Abs(Structure.GetConnection(Joint).InterfaceNormal.Y) > 1.0e-9 ? 1 : 0;
		}

		TestEqual(
			TEXT("fixture: NOT ONE joint in this wall has a Y component in its normal — that is what "
				 "makes it the control for the corner's out-of-plane head"),
			OutOfPlane, 0);

		TestEqual(
			FString::Printf(
				TEXT("fixture: the two beds under the course-1 brick are posed and the earth-to-earth "
					 "head is skipped, so %d joints are posed; %d are"),
				StraightPosedJoints, Posed),
			Posed, StraightPosedJoints);
	}

	TestTrue(
		*FString::Printf(
			TEXT("AND THE LAID WALL IS 3D TOO — the flag is a property of the build, stated once when "
				 "it is opened, not of what happens to be in it. Structure %d reads "
				 "IsThreeDimensional() == false"),
			BuildStructureId),
		Binding->GetStructure().IsThreeDimensional());

	// Three: and Run still reads the LP.

	TestTrue(
		TEXT("the Destroy tab is always live"),
		Controller.OnToolbarButton(EToolbarButtonId::ModeDestroy));

	TestTrue(
		TEXT("fixture: clicking Run on a live build must report that it landed"),
		Controller.OnToolbarButton(EToolbarButtonId::RunStructure));

	Binding = Subsystem.Find(BuildStructureId);

	if (Binding == nullptr)
	{
		AddError(TEXT("the build must survive its own Run"));
		Fixture.End();
		return true;
	}

	{
		const FStructure& Structure = Binding->GetStructure();

		AddInfo(FString::Printf(
			TEXT("after Run: %d piece(s) released, %d min-violation readout solve(s)"),
			SessionCountReleased(*Binding), Structure.GetMinViolationReadoutSolveCount()));

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			AddInfo(SessionDescribeJoint(Structure, Joint));
		}

		TestEqual(
			FString::Printf(
				TEXT("a bonded running-bond wall must still be standing after Run; %d piece(s) went"),
				SessionCountReleased(*Binding)),
			SessionCountReleased(*Binding), 0);

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			if (!SessionJointIsPosedByTheLP(Structure, Joint))
			{
				continue;
			}

			TestTrue(
				*FString::Printf(
					TEXT("A STRAIGHT PLAYER WALL READS THE LP TODAY AND MUST GO ON READING IT: this "
						 "posed bed carries no min-violation readout, which means the gate declined. "
						 "If this line is red while the corner's twin is green, the 3D pose has stopped "
						 "answering for a PLANAR wall and the fix has taken the LP off every straight "
						 "wall a player lays. %s"),
					*SessionDescribeJoint(Structure, Joint)),
				Structure.GetConnectionReadout(Joint).bPresent);
		}
	}

	Fixture.End();

	return true;
}

/**
 * A straight session build is flagged 3D but solved in the planar (2D) pose; a corner is solved 3D.
 *
 * Pins the wiring of Core.Oracle.PlanarProblemUnderThe3DFlagPosesIn2D at the player's Run. Posing
 * every build 3D took a 100-brick wall from 2.5 s to 94 s; timing is not asserted. Observed via
 * GetLastEquilibriumProblemDim(). The flag must stay set (E3 ruling), and the corner build rules out
 * "always 2D".
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionStraightBuildRunsInThePlanarPoseTest,
	"DestructionGame.World.Session.StraightBuildRunsInThePlanarPose",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionStraightBuildRunsInThePlanarPoseTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionControllerTestSupport;

	// The dimensions as the accessor reports them.
	constexpr int32 PosedInTwoD = 2;
	constexpr int32 PosedInThreeD = 3;

	// Course 1's running-bond stagger.
	constexpr double PlanarCourse1XCm = 11.25;

	FSessionFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;
	UBuildModeComponent& Build = *Fixture.Build;
	UDestructionStructureSubsystem& Subsystem = *Fixture.TestWorld.Subsystem;

	if (!Controller.OnToolbarButton(EToolbarButtonId::ModeBuild))
	{
		AddError(TEXT("fixture: the Build tab must be clickable for any of this to run"));
		Fixture.End();
		return true;
	}

	// One: a straight wall, run.

	{
		const int32 StraightId = Build.GetStructureId();

		Controller.PrimaryAlongRay(SessionPointerRayStart(0.0), SessionPointerRayEnd(0.0));
		Controller.PrimaryAlongRay(
			SessionPointerRayStart(SessionSecondCursorXCm),
			SessionPointerRayEnd(SessionSecondCursorXCm));
		Controller.OnToolbarButton(EToolbarButtonId::CourseUp);
		Controller.PrimaryAlongRay(
			SessionPointerRayStart(PlanarCourse1XCm), SessionPointerRayEnd(PlanarCourse1XCm));

		FStructureBinding* const Binding = Subsystem.Find(StraightId);

		if (Binding == nullptr || Binding->NumPieces() != 3)
		{
			AddError(FString::Printf(
				TEXT("fixture: the three clicks must give a three-piece wall; the build holds %d"),
				Binding != nullptr ? Binding->NumPieces() : INDEX_NONE));

			Fixture.End();
			return true;
		}

		int32 OutOfPlane = 0;
		int32 Posed = 0;

		for (int32 Joint = 0; Joint < Binding->GetStructure().NumConnections(); ++Joint)
		{
			AddInfo(SessionDescribeJoint(Binding->GetStructure(), Joint));

			Posed += SessionJointIsPosedByTheLP(Binding->GetStructure(), Joint) ? 1 : 0;
			OutOfPlane +=
				FMath::Abs(Binding->GetStructure().GetConnection(Joint).InterfaceNormal.Y) > 1.0e-9
					? 1 : 0;
		}

		TestEqual(
			TEXT("fixture: not one joint in this wall leaves the X-Z plane — that is what makes it "
				 "the planar case"),
			OutOfPlane, 0);

		TestTrue(
			*FString::Printf(
				TEXT("fixture: the bridge must POSE something (%d joints), or 'every posed joint is "
					 "in-plane' is vacuous"),
				Posed),
			Posed > 0);

		TestTrue(
			TEXT("the Destroy tab is always live"),
			Controller.OnToolbarButton(EToolbarButtonId::ModeDestroy));

		TestTrue(
			TEXT("fixture: clicking Run on a live build must report that it landed"),
			Controller.OnToolbarButton(EToolbarButtonId::RunStructure));

		FStructureBinding* const AfterRun = Subsystem.Find(StraightId);

		if (AfterRun == nullptr)
		{
			AddError(TEXT("the build must survive its own Run"));
			Fixture.End();
			return true;
		}

		const FStructure& Structure = AfterRun->GetStructure();

		AddInfo(FString::Printf(
			TEXT("STRAIGHT after Run: flagged3D %d, posed dim %d, %d released, %d readout solve(s)"),
			Structure.IsThreeDimensional() ? 1 : 0, Structure.GetLastEquilibriumProblemDim(),
			SessionCountReleased(*AfterRun), Structure.GetMinViolationReadoutSolveCount()));

		TestTrue(
			TEXT("THE FLAG MUST STILL BE SET. Un-flagging a build with no corner in it would satisfy "
				 "the dimension assertion below and re-open the hole the E3 ruling closed: the "
				 "authority deciding whether the wall stands would change as a rotated brick landed. "
				 "The flag is the stated PERMISSION to pose 3D; which pose is built is the bridge's "
				 "call, made from what the problem actually contains"),
			Structure.IsThreeDimensional());

		TestEqual(
			*FString::Printf(
				TEXT("[PIN] A STRAIGHT PLAYER WALL MUST RUN IN THE PLANAR POSE. Every joint the bridge "
					 "poses here has an in-plane normal at one Y, so the 3D pose's out-of-plane force "
					 "and moment rows are linear combinations of the in-plane ones and carry nothing. "
					 "The STRENGTH rows are a different story and are why 2D is the ACCURATE pose here "
					 "rather than just the cheap one: the 3D friction pyramid is a k=8 octagon "
					 "INSCRIBED in the Coulomb cone (cos(pi/8) = 0.924), so it caps pure in-plane "
					 "shear at 0.924x the exact limit the 2D rows carry and a shear-critical planar "
					 "wall with lambda* in [1.0, 1.0824) would FALL in 3D and STAND in 2D. 2D is also "
					 "~37x cheaper (2.5 s -> 94 s cold on a 100-brick wall). Posed dim reads %d, where "
					 "2 is required and -1 means no pose was ever stamped"),
				Structure.GetLastEquilibriumProblemDim()),
			Structure.GetLastEquilibriumProblemDim(), PosedInTwoD);
	}

	// Two: the control, a corner still runs in 3D.

	{
		TestTrue(
			TEXT("fixture: back to Build to lay the second structure"),
			Controller.OnToolbarButton(EToolbarButtonId::ModeBuild));

		TestTrue(
			TEXT("fixture: Clear build must be live once something has been laid — it is how this "
				 "test gets a SECOND binding out of one controller"),
			Controller.OnToolbarButton(EToolbarButtonId::ClearBuild));

		const int32 CornerId = Build.GetStructureId();

		// CornerBuildIsJudgedByTheLP's L, duplicated so neither fixture can drift the other.
		struct FPlanarCornerStep
		{
			double CursorXCm;
			double CursorYCm;
			bool bRotated;
			int32 Course;
		};

		const FPlanarCornerStep CornerSteps[] = {
			{  0.000,  0.000, false, 0 },
			{ 16.875,  5.000, true,  0 },
			{ 16.875, 28.000, true,  0 },
			{ 16.875, 50.500, true,  0 },
			{ 16.875, 16.875, true,  1 },
			{ 16.875, 39.375, true,  1 },
		};

		// Clear does not reset the course stepper.
		while (Controller.GetSessionToolbarState().Course > 0)
		{
			Controller.OnToolbarButton(EToolbarButtonId::CourseDown);
		}

		int32 Course = 0;
		bool bRotated = Controller.GetSessionToolbarState().bRotated;

		for (int32 Step = 0; Step < UE_ARRAY_COUNT(CornerSteps); ++Step)
		{
			const FPlanarCornerStep& Lay = CornerSteps[Step];

			if (Lay.bRotated != bRotated)
			{
				Controller.OnToolbarButton(EToolbarButtonId::RotatePiece);
				bRotated = Lay.bRotated;
			}

			while (Course < Lay.Course)
			{
				Controller.OnToolbarButton(EToolbarButtonId::CourseUp);
				++Course;
			}

			if (!Controller.PrimaryAlongRay(
				SessionPointerRayStartAt(Lay.CursorXCm, Lay.CursorYCm),
				SessionPointerRayEndAt(Lay.CursorXCm, Lay.CursorYCm)))
			{
				AddError(FString::Printf(
					TEXT("fixture: corner click %d at (%g, %g) must lay a brick"),
					Step, Lay.CursorXCm, Lay.CursorYCm));
			}
		}

		FStructureBinding* Binding = Subsystem.Find(CornerId);

		if (Binding == nullptr || Binding->NumPieces() != 6)
		{
			AddError(FString::Printf(
				TEXT("fixture: the six clicks must give a six-piece L; the build holds %d"),
				Binding != nullptr ? Binding->NumPieces() : INDEX_NONE));

			Fixture.End();
			return true;
		}

		int32 PosedOutOfPlane = 0;

		for (int32 Joint = 0; Joint < Binding->GetStructure().NumConnections(); ++Joint)
		{
			AddInfo(SessionDescribeJoint(Binding->GetStructure(), Joint));

			if (SessionJointIsPosedByTheLP(Binding->GetStructure(), Joint)
				&& FMath::Abs(Binding->GetStructure().GetConnection(Joint).InterfaceNormal.Y) > 1.0e-9)
			{
				++PosedOutOfPlane;
			}
		}

		TestEqual(
			*FString::Printf(
				TEXT("FIXTURE, AND THE WHOLE POINT OF THE TWO COURSE-1 BRICKS: exactly one POSED "
					 "out-of-plane joint, the head 5-4 between two pieces neither of which is "
					 "grounded. An L laid entirely on the earth has every Y-normal joint skipped as "
					 "earth-to-earth and would legitimately pose PLANAR — so without this the control "
					 "would be measuring the same case as section ONE. It holds %d"),
				PosedOutOfPlane),
			PosedOutOfPlane, 1);

		TestTrue(
			TEXT("the Destroy tab is always live"),
			Controller.OnToolbarButton(EToolbarButtonId::ModeDestroy));

		TestTrue(
			TEXT("fixture: clicking Run on the corner build must report that it landed"),
			Controller.OnToolbarButton(EToolbarButtonId::RunStructure));

		Binding = Subsystem.Find(CornerId);

		if (Binding == nullptr)
		{
			AddError(TEXT("the corner build must survive its own Run"));
			Fixture.End();
			return true;
		}

		const FStructure& Structure = Binding->GetStructure();

		AddInfo(FString::Printf(
			TEXT("CORNER after Run: flagged3D %d, posed dim %d, %d released, %d readout solve(s)"),
			Structure.IsThreeDimensional() ? 1 : 0, Structure.GetLastEquilibriumProblemDim(),
			SessionCountReleased(*Binding), Structure.GetMinViolationReadoutSolveCount()));

		TestTrue(TEXT("the corner build is flagged 3D, exactly as the straight one is"),
			Structure.IsThreeDimensional());

		TestEqual(
			*FString::Printf(
				TEXT("[NET] AND A CORNER MUST STILL RUN IN 3D. This is the half 'always pose 2D' would "
					 "break: the 2D X-Z oracle cannot express the Y-facing head joint at all, so it "
					 "would be answered by a projection with wrong statics — a plausible number rather "
					 "than a slow one. Posed dim reads %d, where 3 is required and -1 means no pose "
					 "was ever stamped"),
				Structure.GetLastEquilibriumProblemDim()),
			Structure.GetLastEquilibriumProblemDim(), PosedInThreeD);
	}

	Fixture.End();

	return true;
}

/**
 * A toolbar click (Rotate, piece, Free, Course up) moves the ghost at once, with no pointer event.
 *
 * Drives the controller's door because OnToolbarButton writes PlacementMode as a bare field; the
 * component's setters are pinned in World.BuildMode.SettingsChangeRefreshesTheHeldPreview. Asserted
 * on ghost bounds only. The cursor at (11.25, 3.0) is off-grid so corner poses are not tied.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarChangesMoveTheGhostAtOnceTest,
	"DestructionGame.World.Session.ToolbarChangesMoveTheGhostAtOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarChangesMoveTheGhostAtOnceTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionControllerTestSupport;

	// Footprints spelled out, not asked of the palette.
	const FVector UprightBrickSizeCm(21.5, 10.25, 6.5);
	const FVector RotatedBrickSizeCm(10.25, 21.5, 6.5);
	const FVector UprightPlateSizeCm(67.5, 10.25, 10.0);

	// The cursor and the three poses the ghost must take there.
	constexpr double CursorXCm = 11.25;
	constexpr double CursorYCm = 3.0;

	const FVector NextCourseCentreCm(11.25, 0.0, 10.75);
	const FVector FreeAtCourse0Cm(11.25, 3.0, 3.25);
	const FVector FreeAtCourse1Cm(11.25, 3.0, 10.75);

	FSessionFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;
	UBuildModeComponent& Build = *Fixture.Build;

	if (!Controller.OnToolbarButton(EToolbarButtonId::ModeBuild))
	{
		AddError(TEXT("fixture: the Build tab must be clickable for any of this to run"));
		Fixture.End();
		return true;
	}

	// bNonColliding bounds, since the ghost's collision is disabled.
	const auto GhostBounds = [this, &Build]() -> FBox
	{
		AActor* const Ghost = Build.GetGhostActor();

		if (Ghost == nullptr)
		{
			AddError(TEXT("there is no ghost actor to read — pointing in Build mode must pose one"));
			return FBox(ForceInit);
		}

		return Ghost->GetComponentsBoundingBox(/*bNonColliding*/ true);
	};

	// One: a seed brick, then a pointer move that holds a preview.

	{
		const bool bPlaced = Controller.PrimaryAlongRay(
			SessionPointerRayStart(0.0), SessionPointerRayEnd(0.0));

		TestTrue(
			FString::Printf(TEXT("fixture: the seed brick must land at the origin; the click "
								 "reported %d"),
				bPlaced ? 1 : 0),
			bPlaced);

		Controller.PointerAlongRay(
			SessionPointerRayStartAt(CursorXCm, CursorYCm),
			SessionPointerRayEndAt(CursorXCm, CursorYCm));

		const FBox Bounds = GhostBounds();

		TestTrue(
			*FString::Printf(
				TEXT("fixture: pointing beside the seed must stand the ghost on the running-bond "
					 "next-course pose (11.25, 0, 10.75); it is at (%g, %g, %g)"),
				Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z),
			Bounds.GetCenter().Equals(NextCourseCentreCm, BoundsToleranceCm));

		TestTrue(
			*FString::Printf(
				TEXT("fixture: and it must be an upright brick, 21.5 x 10.25 x 6.5; it is "
					 "(%g, %g, %g)"),
				Bounds.GetSize().X, Bounds.GetSize().Y, Bounds.GetSize().Z),
			Bounds.GetSize().Equals(UprightBrickSizeCm, BoundsToleranceCm));
	}

	// Two: Rotate swaps the ghost's footprint. The pose is the solver's business, so only logged.
	{
		TestTrue(
			TEXT("fixture: the Rotate chip is clickable in every Build state"),
			Controller.OnToolbarButton(EToolbarButtonId::RotatePiece));

		const FBox Bounds = GhostBounds();

		AddInfo(FString::Printf(
			TEXT("after Rotate the ghost is (%.4f, %.4f, %.4f) sized (%.4f, %.4f, %.4f)"),
			Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z,
			Bounds.GetSize().X, Bounds.GetSize().Y, Bounds.GetSize().Z));

		TestTrue(
			*FString::Printf(
				TEXT("THE ROTATE CHIP MUST TURN THE GHOST AT ONCE — 10.25 x 21.5 x 6.5, with no "
					 "second pointer event. A chip that lights while the ghost keeps its old "
					 "footprint is the owner's complaint exactly. It is (%g, %g, %g)"),
				Bounds.GetSize().X, Bounds.GetSize().Y, Bounds.GetSize().Z),
			Bounds.GetSize().Equals(RotatedBrickSizeCm, BoundsToleranceCm));
	}

	// Three: a piece chip redraws it as the new piece.

	{
		TestTrue(
			TEXT("fixture: turn the piece back upright before choosing another"),
			Controller.OnToolbarButton(EToolbarButtonId::RotatePiece));

		TestTrue(
			TEXT("fixture: a piece chip is always live in Build mode"),
			Controller.OnToolbarButton(EToolbarButtonId::PieceTimberPlate));

		const FBox Bounds = GhostBounds();

		AddInfo(FString::Printf(
			TEXT("after the plate chip the ghost is (%.4f, %.4f, %.4f) sized (%.4f, %.4f, %.4f)"),
			Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z,
			Bounds.GetSize().X, Bounds.GetSize().Y, Bounds.GetSize().Z));

		TestTrue(
			*FString::Printf(
				TEXT("THE PIECE CHIP MUST RE-DRAW THE GHOST AT ONCE — the demo's 67.5 x 10.25 x 10 "
					 "plate, with no second pointer event. It is (%g, %g, %g)"),
				Bounds.GetSize().X, Bounds.GetSize().Y, Bounds.GetSize().Z),
			Bounds.GetSize().Equals(UprightPlateSizeCm, BoundsToleranceCm));
	}

	// Four: Free drops the ghost onto the cursor.

	{
		TestTrue(
			TEXT("fixture: back to a brick"),
			Controller.OnToolbarButton(EToolbarButtonId::PieceBrick));

		TestTrue(
			TEXT("fixture: the Free chip is live in Build mode"),
			Controller.OnToolbarButton(EToolbarButtonId::PlacementFree));

		const FBox Bounds = GhostBounds();

		TestTrue(
			*FString::Printf(
				TEXT("THE FREE CHIP MUST MOVE THE GHOST AT ONCE: Free honours the cursor verbatim, so "
					 "the ghost must leave the snapped pose and stand at the cursor's point on the "
					 "course-0 plane, (11.25, 3, 3.25). It is at (%g, %g, %g)"),
				Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z),
			Bounds.GetCenter().Equals(FreeAtCourse0Cm, BoundsToleranceCm));
	}

	// Five: Course up lifts it by exactly one course.

	{
		TestTrue(
			TEXT("fixture: Course up is live in Build mode"),
			Controller.OnToolbarButton(EToolbarButtonId::CourseUp));

		TestEqual(
			*FString::Printf(TEXT("fixture: the session must be on course 1; the state is %s"),
				*SessionStateBits(Controller.GetSessionToolbarState())),
			Controller.GetSessionToolbarState().Course, 1);

		const FBox Bounds = GhostBounds();

		TestTrue(
			*FString::Printf(
				TEXT("THE COURSE CHIP MUST LIFT THE GHOST AT ONCE, ONTO THE NEW BUILD PLANE: a brick "
					 "on course 1 rests at 7.5 + 3.25 = 10.75, so the Free ghost must rise exactly "
					 "one course to (11.25, 3, 10.75). It is at (%g, %g, %g)"),
				Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z),
			Bounds.GetCenter().Equals(FreeAtCourse1Cm, BoundsToleranceCm));
	}

	// No settings click may commit anything.
	{
		UDestructionStructureSubsystem& Subsystem = *Fixture.TestWorld.Subsystem;

		if (FStructureBinding* const Binding = Subsystem.Find(Build.GetStructureId()))
		{
			TestEqual(
				FString::Printf(
					TEXT("a settings click must never place a piece; the plot holds %d"),
					Binding->NumPieces()),
				Binding->NumPieces(), 1);
		}
	}

	Fixture.End();

	return true;
}

/**
 * The per-tick cursor refresh drives the ghost from a ray in Build mode, and does nothing in Destroy.
 *
 * Tested through RefreshBuildPreviewFromRay because mouse deprojection needs a viewport. A refresh
 * leaking into Destroy would show a ghost over the wall and re-arm a preview a stray confirm could
 * commit. The look-chord guard is left to playtest.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionCursorRefreshDrivesTheGhostFromARayTest,
	"DestructionGame.World.Session.CursorRefreshDrivesTheGhostFromARay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionCursorRefreshDrivesTheGhostFromARayTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionControllerTestSupport;

	const FVector UprightBrickSizeCm(21.5, 10.25, 6.5);
	const FVector NextCourseCentreCm(11.25, 0.0, 10.75);

	// Straight down through (11.25, 3.0).
	const FVector CursorRayOriginCm(11.25, 3.0, SessionRayStartZCm);
	const FVector CursorRayDirection(0.0, 0.0, -1.0);

	FSessionFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;
	UBuildModeComponent& Build = *Fixture.Build;
	UDestructionStructureSubsystem& Subsystem = *Fixture.TestWorld.Subsystem;

	if (!Controller.OnToolbarButton(EToolbarButtonId::ModeBuild))
	{
		AddError(TEXT("fixture: the Build tab must be clickable for any of this to run"));
		Fixture.End();
		return true;
	}

	const int32 BuildStructureId = Build.GetStructureId();

	// One: a seed brick to snap against.
	{
		const bool bPlaced = Controller.PrimaryAlongRay(
			SessionPointerRayStart(0.0), SessionPointerRayEnd(0.0));

		TestTrue(
			FString::Printf(TEXT("fixture: the seed brick must land at the origin; the click "
								 "reported %d"),
				bPlaced ? 1 : 0),
			bPlaced);
	}

	// Two: in Build mode the refresh puts the ghost where the ray points.

	{
		const bool bRefreshed = Controller.RefreshBuildPreviewFromRay(
			CursorRayOriginCm, CursorRayDirection);

		TestTrue(
			TEXT("A CURSOR REFRESH IN BUILD MODE MUST PUT A GHOST UP — this is what shows the player "
				 "where the brick is going to go before they click anything"),
			bRefreshed);

		AActor* const Ghost = Build.GetGhostActor();

		TestNotNull(
			TEXT("and it must have posed the component's ghost"),
			Ghost);

		if (Ghost != nullptr)
		{
			TestFalse(
				TEXT("the ghost must be VISIBLE — a preview nobody can see is not a preview"),
				Ghost->IsHidden());

			const FBox Bounds = Ghost->GetComponentsBoundingBox(/*bNonColliding*/ true);

			TestTrue(
				*FString::Printf(
					TEXT("AND AT THE POSE A POINTER MOVE WOULD GIVE: the running-bond next-course "
						 "snap (11.25, 0, 10.75) beside the seed. It is at (%g, %g, %g)"),
					Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z),
				Bounds.GetCenter().Equals(NextCourseCentreCm, BoundsToleranceCm));

			TestTrue(
				*FString::Printf(
					TEXT("and it is an upright brick, 21.5 x 10.25 x 6.5; it is (%g, %g, %g)"),
					Bounds.GetSize().X, Bounds.GetSize().Y, Bounds.GetSize().Z),
				Bounds.GetSize().Equals(UprightBrickSizeCm, BoundsToleranceCm));
		}
	}

	// Three: in Destroy mode it does nothing.

	{
		TestTrue(
			TEXT("fixture: the Destroy tab must be clickable"),
			Controller.OnToolbarButton(EToolbarButtonId::ModeDestroy));

		AActor* const Ghost = Build.GetGhostActor();

		if (Ghost != nullptr)
		{
			TestTrue(
				TEXT("fixture: leaving Build mode hides the ghost, so the refusal below is a ghost "
					 "that STAYS hidden rather than one that was never up"),
				Ghost->IsHidden());
		}

		const bool bRefreshed = Controller.RefreshBuildPreviewFromRay(
			CursorRayOriginCm, CursorRayDirection);

		TestFalse(
			TEXT("A CURSOR REFRESH IN DESTROY MODE MUST DO NOTHING AND SAY SO. Run every tick, a "
				 "refresh that leaked the mode would put a gold ghost over the wall the player is "
				 "demolishing and re-arm a preview a stray confirm could commit"),
			bRefreshed);

		if (Ghost != nullptr)
		{
			TestTrue(
				TEXT("and the ghost must still be hidden"),
				Ghost->IsHidden());
		}

		if (FStructureBinding* const Binding = Subsystem.Find(BuildStructureId))
		{
			TestEqual(
				FString::Printf(
					TEXT("and nothing may have been placed; the plot holds %d pieces"),
					Binding->NumPieces()),
				Binding->NumPieces(), 1);
		}
	}

	Fixture.End();

	return true;
}

/**
 * A click must re-drive the ghost along the same ray, with no further pointer event.
 *
 * With a still mouse the ghost was left inside the brick just laid, z-fighting it, because nothing
 * re-previewed after ConfirmPlace. After a click at ray R the ghost must be visible, not at the
 * placed centre, equal to a fresh preview at R (22.5, 0, 3.25), and held (so the next click commits
 * it); the re-preview must not commit anything.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionPlacingReDrivesTheGhostAlongTheSameRayTest,
	"DestructionGame.World.Session.PlacingReDrivesTheGhostAlongTheSameRay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionPlacingReDrivesTheGhostAlongTheSameRayTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionControllerTestSupport;

	// R: straight down through (11.25, 3.0).
	constexpr double CursorXCm = 11.25;
	constexpr double CursorYCm = 3.0;

	const FVector CursorRayOriginCm(CursorXCm, CursorYCm, SessionRayStartZCm);
	const FVector CursorRayDirection(0.0, 0.0, -1.0);

	// What the click commits, and where the ghost must stand afterwards.
	const FVector NextCourseCentreCm(11.25, 0.0, 10.75);
	const FVector SameCourseCentreCm(22.5, 0.0, 3.25);

	FSessionFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;
	UBuildModeComponent& Build = *Fixture.Build;
	UDestructionStructureSubsystem& Subsystem = *Fixture.TestWorld.Subsystem;

	if (!Controller.OnToolbarButton(EToolbarButtonId::ModeBuild))
	{
		AddError(TEXT("fixture: the Build tab must be clickable for any of this to run"));
		Fixture.End();
		return true;
	}

	const int32 BuildStructureId = Build.GetStructureId();

	// bNonColliding bounds, since the ghost's collision is disabled.
	const auto GhostBounds = [this, &Build]() -> FBox
	{
		AActor* const Ghost = Build.GetGhostActor();

		if (Ghost == nullptr)
		{
			AddError(TEXT("there is no ghost actor to read — pointing in Build mode must pose one"));
			return FBox(ForceInit);
		}

		return Ghost->GetComponentsBoundingBox(/*bNonColliding*/ true);
	};

	// One: a seed brick to snap against.
	{
		const bool bPlaced = Controller.PrimaryAlongRay(
			SessionPointerRayStart(0.0), SessionPointerRayEnd(0.0));

		TestTrue(
			FString::Printf(TEXT("fixture: the seed brick must land at the origin; the click "
								 "reported %d"),
				bPlaced ? 1 : 0),
			bPlaced);
	}

	// Two: the cursor refresh at R puts a ghost up, as the tick would.

	{
		const bool bRefreshed = Controller.RefreshBuildPreviewFromRay(
			CursorRayOriginCm, CursorRayDirection);

		TestTrue(
			TEXT("fixture: a cursor refresh in Build mode must hold a valid preview at R — this "
				 "test is about what the CLICK does to that ghost, so it has to be up first"),
			bRefreshed);

		const FBox Bounds = GhostBounds();

		TestTrue(
			*FString::Printf(
				TEXT("fixture: and it must be on the running-bond next-course snap (11.25, 0, 10.75), "
					 "8.08 cm from the cursor's plane point against 11.64 cm for the same-course "
					 "pose. It is at (%g, %g, %g)"),
				Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z),
			Bounds.GetCenter().Equals(NextCourseCentreCm, BoundsToleranceCm));
	}

	// Three: the click commits that pose.

	FVector PlacedCentreCm = FVector::ZeroVector;

	{
		const bool bPlaced = Controller.PrimaryAlongRay(
			SessionPointerRayStartAt(CursorXCm, CursorYCm),
			SessionPointerRayEndAt(CursorXCm, CursorYCm));

		TestTrue(
			FString::Printf(TEXT("fixture: the click at R must lay a piece; it reported %d"),
				bPlaced ? 1 : 0),
			bPlaced);

		FStructureBinding* const Binding = Subsystem.Find(BuildStructureId);

		if (Binding == nullptr || Binding->NumPieces() != 2)
		{
			AddError(FString::Printf(
				TEXT("fixture: the plot must hold the seed and the clicked brick; it holds %d"),
				Binding != nullptr ? Binding->NumPieces() : INDEX_NONE));
			Fixture.End();
			return true;
		}

		PlacedCentreCm = Binding->GetBinding(1).Box.CentreCm;

		TestTrue(
			*FString::Printf(
				TEXT("fixture: and it must have landed on the pose the ghost was showing, "
					 "(11.25, 0, 10.75); it is at (%g, %g, %g)"),
				PlacedCentreCm.X, PlacedCentreCm.Y, PlacedCentreCm.Z),
			PlacedCentreCm.Equals(NextCourseCentreCm, BoundsToleranceCm));
	}

	// Four: with no further call the ghost must have moved on.
	FVector GhostCentreAfterClickCm = FVector::ZeroVector;

	{
		// First, so a re-drive that placed and undid is caught.
		if (const FStructureBinding* const Binding = Subsystem.Find(BuildStructureId))
		{
			TestEqual(
				FString::Printf(
					TEXT("THE RE-PREVIEW MUST NOT COMMIT: the click laid exactly one piece, so the "
						 "plot holds the seed and it. It holds %d"),
					Binding->NumPieces()),
				Binding->NumPieces(), 2);
		}

		AActor* const Ghost = Build.GetGhostActor();

		TestNotNull(TEXT("the click must leave the component's ghost in the world"), Ghost);

		if (Ghost == nullptr)
		{
			Fixture.End();
			return true;
		}

		TestFalse(
			TEXT("THE GHOST MUST STILL BE VISIBLE AFTER THE CLICK — hiding it on commit answers the "
				 "z-fight by leaving the player with no preview at all until they jog the mouse, "
				 "which is the same complaint from the other side"),
			Ghost->IsHidden());

		GhostCentreAfterClickCm = GhostBounds().GetCenter();

		AddInfo(FString::Printf(
			TEXT("after the click the ghost is at (%.4f, %.4f, %.4f); the brick it just laid is at "
				 "(%.4f, %.4f, %.4f)"),
			GhostCentreAfterClickCm.X, GhostCentreAfterClickCm.Y, GhostCentreAfterClickCm.Z,
			PlacedCentreCm.X, PlacedCentreCm.Y, PlacedCentreCm.Z));

		TestFalse(
			*FString::Printf(
				TEXT("AND IT MUST NOT BE STANDING INSIDE THE BRICK IT JUST LAID. With the mouse "
					 "still, nothing re-previews after the commit, so the gold ghost z-fights the "
					 "red brick at the placed centre until the pointer moves a pixel — the one "
					 "moment 'show where the brick is going to go' is not honoured. The ghost is at "
					 "(%g, %g, %g) and the piece at (%g, %g, %g)"),
				GhostCentreAfterClickCm.X, GhostCentreAfterClickCm.Y, GhostCentreAfterClickCm.Z,
				PlacedCentreCm.X, PlacedCentreCm.Y, PlacedCentreCm.Z),
			GhostCentreAfterClickCm.Equals(PlacedCentreCm, BoundsToleranceCm));

		// Held, not just moved: RefreshPreview reads the flag ConfirmPlace clears.
		TestTrue(
			TEXT("AND A VALID PREVIEW MUST BE HELD: the ghost showing the next pose is only honest if "
				 "a confirm would commit THAT pose. ConfirmPlace spends the held preview and nothing "
				 "re-arms it, so today there is a ghost on screen and no preview behind it"),
			Build.RefreshPreview());
	}

	/*
	 * Five: a fresh preview at R, asked after the snapshot so the comparison is not circular. Its
	 * answer is also pinned by hand.
	 */
	{
		const FBuildPreview Oracle = Build.UpdatePreviewFromRay(
			CursorRayOriginCm, CursorRayDirection);

		TestTrue(
			TEXT("fixture: a fresh preview at R must be valid — the pose beside the seed is free"),
			Oracle.bValid);

		AddInfo(FString::Printf(
			TEXT("a fresh preview at the same ray answers (%.4f, %.4f, %.4f)"),
			Oracle.CentreCm.X, Oracle.CentreCm.Y, Oracle.CentreCm.Z));

		TestTrue(
			*FString::Printf(
				TEXT("fixture: and that answer must be the same-course running-bond pose beside the "
					 "seed, (22.5, 0, 3.25) — the next-course pose is now occupied and dropped, and "
					 "every other live pose is 19 cm or further out. It is (%g, %g, %g)"),
				Oracle.CentreCm.X, Oracle.CentreCm.Y, Oracle.CentreCm.Z),
			Oracle.CentreCm.Equals(SameCourseCentreCm, BoundsToleranceCm));

		TestTrue(
			*FString::Printf(
				TEXT("THE GHOST THE CLICK LEFT MUST BE STANDING EXACTLY THERE — the click has to "
					 "re-drive the preview along the same ray, so that what the player sees with a "
					 "still mouse is what the next click would lay. The ghost was at (%g, %g, %g); "
					 "the fresh answer is (%g, %g, %g)"),
				GhostCentreAfterClickCm.X, GhostCentreAfterClickCm.Y, GhostCentreAfterClickCm.Z,
				Oracle.CentreCm.X, Oracle.CentreCm.Y, Oracle.CentreCm.Z),
			GhostCentreAfterClickCm.Equals(Oracle.CentreCm, BoundsToleranceCm));
	}

	// Six: the next click lays a piece at the pose the ghost was showing.

	{
		const bool bPlaced = Controller.PrimaryAlongRay(
			SessionPointerRayStartAt(CursorXCm, CursorYCm),
			SessionPointerRayEndAt(CursorXCm, CursorYCm));

		TestTrue(
			FString::Printf(TEXT("a second click at the same R must lay a second piece; it "
								 "reported %d"),
				bPlaced ? 1 : 0),
			bPlaced);

		const FStructureBinding* const Binding = Subsystem.Find(BuildStructureId);

		if (Binding == nullptr || Binding->NumPieces() != 3)
		{
			AddError(FString::Printf(
				TEXT("the plot must now hold three pieces; it holds %d"),
				Binding != nullptr ? Binding->NumPieces() : INDEX_NONE));
			Fixture.End();
			return true;
		}

		const FVector SecondCentreCm = Binding->GetBinding(2).Box.CentreCm;

		TestTrue(
			*FString::Printf(
				TEXT("AND IT MUST LAND WHERE THE GHOST HAD BEEN STANDING SINCE THE FIRST CLICK, "
					 "(22.5, 0, 3.25) — which is what makes the re-preview a genuinely held preview "
					 "rather than a moved actor. It is at (%g, %g, %g)"),
				SecondCentreCm.X, SecondCentreCm.Y, SecondCentreCm.Z),
			SecondCentreCm.Equals(SameCourseCentreCm, BoundsToleranceCm));
	}

	Fixture.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
