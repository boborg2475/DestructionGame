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
 * SESSION S4/S5 — the controller is the session: the toolbar is the only door into its state, and
 * the mode decides what a ray does.
 *
 * `ADestructionGamePlayerController` holds one `FSessionToolbarState`, changes it only through
 * `OnToolbarButton` (which pushes the change onto its own `UBuildModeComponent` and refuses
 * whatever the model refuses, side effects included), and dispatches a world ray by mode — Build
 * previews and places, Destroy hovers and inspects.
 *
 * `Core/SessionToolbar.h` already owns which buttons exist, which are greyed, and what one click
 * does to the state (`ApplyToolbarButton` is pure and `Core.SessionToolbar.*` sweeps it); what no
 * model can own is the side effects. A click that moves the course must move the component's build
 * plane, and a click the model refuses must move nothing — a controller that applied the
 * transition and then ran its side effect unconditionally would have a greyed `Course down`
 * quietly lowering the build plane below the earth while the readout still said course 0.
 * CURRENT_STATE records the component's kind, course and placement as a parallel copy of three
 * state fields for exactly this reason; this file pins who owns them.
 *
 * Every claim here is a mechanism reading: the returned bool, the state's own fields, the
 * component's structure id/piece kind/course/build plane, the binding's piece and connection
 * counts, `IsPieceMenuShown`, a brick's `GetHighlight`, and `IsReleased` — never a distance moved
 * (DESIGN §4). This bites hardest in `RunStructureSettlesTheBuild`, where the Free brick is
 * released 45 cm above the floor and would be measured falling by a physics tick nobody runs;
 * `IsReleased` answers "did Run do its job", not where the brick ends up.
 *
 * A world ticks in the sense that the subsystem spawns real `ABrickActor`s, the ghost is an actor,
 * and the Destroy ray is a real line trace against real collision — but no test here ticks one,
 * since nothing is about anything moving.
 *
 * Named namespace, deliberately different from every other one in this module: an anonymous
 * namespace is private to a translation unit rather than a file, and a unity build merges many
 * files into one (see CURRENT_STATE.md). The `using namespace` lives inside each RunTest body for
 * the same reason.
 */
namespace SessionControllerTestSupport
{
	using namespace DestructionSession;

	/*
	 * The rests-on-the-ground arithmetic, spelled out rather than imported (DESIGN §8, 2026-09-15).
	 *
	 * A brick is 21.5 x 10.25 x 6.5 cm on a 1 cm joint, so the coordinating grid is 22.5 x 11.25 x
	 * 7.5 and the brick's own half height is 3.25. Course 0 therefore centres a brick at 3.25 with
	 * its underside on the earth; course n centres it at n * 7.5 + 3.25. A timber plate is 5.0 cm
	 * half-height, so the same course puts it at n * 7.5 + 5.0 — the reason the plane is derived
	 * from the piece as well as the course.
	 *
	 * Calling DestructionSession::CoursePlaneZCm here would make these tests agree with the plane
	 * function however wrong it is, which is the one thing they exist to catch.
	 */

	/** Course 0's plane for a brick: 0 * 7.5 + 3.25. */
	constexpr double SessionBrickPlaneCourse0Cm = 3.25;

	/** Course 2's plane for a TIMBER PLATE: 2 * 7.5 + 5.0 — where the demo building puts its plate. */
	constexpr double SessionPlatePlaneCourse2Cm = 20.0;

	/** Course 0's plane for a plate: 0 * 7.5 + 5.0. */
	constexpr double SessionPlatePlaneCourse0Cm = 5.0;

	/** The half extent of the demo's timber plate, which SetPieceKind must derive. */
	const FVector SessionPlateHalfExtentCm(33.75, 5.125, 5.0);

	/**
	 * THE COURSE THE FLOATING BRICK IN THE RUN TEST IS LAID ON, and its plane.
	 *
	 * 6 * 7.5 + 3.25 = 48.25, so the brick's bottom face is at 45 cm — forty-five times the 1 cm
	 * joint the grounded rule allows, so it cannot read grounded by accident. It is also five brick
	 * lengths clear of the seed along X, which is well outside the 30 cm snap radius, so the Free
	 * placement is jointless for two independent reasons rather than one.
	 */
	constexpr int32 SessionFloatingCourse = 6;
	constexpr double SessionFloatingPlaneZCm = 48.25;
	constexpr double SessionFloatingXCm = 100.0;

	/**
	 * THE TWO POSES THE BUILD-THEN-DESTROY TEST LAYS, derived from the same grid.
	 *
	 * The seed goes at the origin on course 0. The second cursor is aimed at x = 22.0 — deliberately
	 * OFF the grid — and the running bond's same-course pose beside the seed is one brick plus one
	 * joint away at x = 22.5, which is 0.5 cm from the cursor. The nearest next-course pose is at
	 * (11.25, 0, 10.75), which is sqrt(10.75^2 + 7.5^2) = 13.1 cm away, so the same-course snap wins
	 * on distance by a factor of 26 and the fixture is nowhere near its own boundary.
	 */
	const FVector SessionSeedCentreCm(0.0, 0.0, SessionBrickPlaneCourse0Cm);
	constexpr double SessionSecondCursorXCm = 22.0;
	const FVector SessionSecondCentreCm(22.5, 0.0, SessionBrickPlaneCourse0Cm);

	/** And a third, one more bay along: 22.5 + 22.5. Clear of the tombstone the delete leaves at 0. */
	constexpr double SessionThirdCursorXCm = 45.0;

	/**
	 * WHERE THE PLAYER LAYS THEIR ONE BRICK ON A LEVEL THAT ALREADY HAS A WALL ON IT, in cm along X.
	 *
	 * THIRTY METRES CLEAR OF THAT WALL, AND THAT IS A REQUIREMENT RATHER THAN TIDINESS. The sandbox
	 * row lays 30 bricks per course on the 22.5 cm grid at Y = 0, so its courses span X -10.75 to
	 * 663.25 and its 40 courses reach Z = 300. The Destroy ray in that test is a REAL LINE TRACE, so
	 * a brick laid anywhere inside that span could put one of the level's own bricks under the cursor
	 * instead of the player's — and the test would then delete a piece of the wall and prove nothing.
	 */
	constexpr double SessionClearOfTheWallXCm = 3000.0;

	/**
	 * How far above the build plane a pointing ray starts, and how far below it ends.
	 *
	 * THE RAY IS A DIRECTION, NOT A POINT. The controller turns (Start, End) into a direction and
	 * lets the component intersect it with the build plane, so the END's own Z is irrelevant — which
	 * is exactly why it is taken to Z = 0 rather than to the plane. A test that aimed the end AT the
	 * plane would pass against a controller that ignored the plane entirely and used the end point.
	 */
	constexpr double SessionRayStartZCm = 300.0;
	constexpr double SessionRayEndZCm = 0.0;

	/**
	 * How far along Y a DESTROY ray starts and ends, either side of the brick.
	 *
	 * A brick is 10.25 cm deep centred on Y = 0, so +/- 100 cm is far outside it on both sides and
	 * the ray crosses the whole thickness. Along Y so nothing else in the build is in the way. Same
	 * reach the piece-menu tests use.
	 */
	constexpr double SessionInspectReachCm = 100.0;

	FVector SessionPointerRayStart(double XCm)
	{
		return FVector(XCm, 0.0, SessionRayStartZCm);
	}

	FVector SessionPointerRayEnd(double XCm)
	{
		return FVector(XCm, 0.0, SessionRayEndZCm);
	}

	/*
	 * THE SAME RAY AIMED ANYWHERE IN PLAN, for the corner builds below — a leg that runs along Y
	 * cannot be aimed at with a Y = 0 cursor. Straight down from 300 cm, so the component's
	 * ray-vs-plane intersection lands on (XCm, YCm) whatever course the plane is on.
	 */
	FVector SessionPointerRayStartAt(double XCm, double YCm)
	{
		return FVector(XCm, YCm, SessionRayStartZCm);
	}

	FVector SessionPointerRayEndAt(double XCm, double YCm)
	{
		return FVector(XCm, YCm, SessionRayEndZCm);
	}

	/**
	 * Whether the rigid-block bridge will pose this joint at all.
	 *
	 * A joint between two grounded pieces is skipped by the bridge, not posed and not refused:
	 * `BuildRigidBlockProblem` drops it before it ever looks at the normal. That skip is why the
	 * corner fixture below is six pieces rather than three — an L laid entirely on the earth has
	 * every Y-normal head joint between two grounded pieces, so the 2D bridge never reaches the
	 * refusal and a test built on it would be green for a reason that has nothing to do with the
	 * flag. It is also why the readout assertions here run only over the joints this predicate
	 * admits: a skipped joint has no provenance entry and therefore no readout, whatever the dimension.
	 */
	bool SessionJointIsPosedByTheLP(const FStructure& Structure, int32 Connection)
	{
		const FConnection& Joint = Structure.GetConnection(Connection);

		return !(Structure.GetPiece(Joint.PieceA).bIsGrounded
			&& Structure.GetPiece(Joint.PieceB).bIsGrounded);
	}

	/** One joint on one line — pieces, normal, which ends are grounded, and its LP readout. */
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

	/** How many pieces the binding has released — `IsReleased`, never a distance moved. */
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

	/**
	 * Whether two states are the same state, field for field.
	 *
	 * Field by field rather than memcmp, because a struct with a bool in it carries padding and two
	 * states that differ only in their padding are the same state. The model's own refusal contract
	 * is a bitwise no-op, and this is the honest reading of it.
	 */
	bool SessionSameState(const FSessionToolbarState& A, const FSessionToolbarState& B)
	{
		return A.Mode == B.Mode
			&& A.Piece == B.Piece
			&& A.Placement == B.Placement
			&& A.Course == B.Course
			&& A.bHasStructure == B.bHasStructure
			&& A.bRotated == B.bRotated;
	}

	/**
	 * Whether the strip this state draws offers this button live.
	 *
	 * Asked of the production model rather than re-decided here: the claim is that the controller's
	 * state is one the strip can grey correctly, not that this file agrees with the model about
	 * greying. A button the strip does not draw at all answers false.
	 */
	bool SessionButtonIsEnabled(const FSessionToolbarState& State, EToolbarButtonId Id)
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);

		const FToolbarButton* const Button = Buttons.FindByPredicate(
			[Id](const FToolbarButton& Candidate) { return Candidate.Id == Id; });

		return Button != nullptr && Button->bEnabled;
	}

	/** How many ABrickActors stand in the world that are NOT the component's ghost. */
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
	 * A controller in the world with a real ULocalPlayer, and its build component, or nulls.
	 *
	 * The local player is not decoration: `bShowMouseCursor` is a plain field and would flip without
	 * one, but `SetSessionControls` also sets the input mode and the session's mapping contexts are
	 * applied through the Enhanced Input local-player subsystem — a controller with no local player
	 * has none, so the engine never runs `SetupInputComponent` for it either. A fixture without one
	 * would assert the cursor half of the session's controls while the input half silently failed
	 * closed.
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
	 * Spawn the controller and a pawn the way `UEngine::LoadMap` does, between actor init and
	 * begin-play — so a game mode's begin-play runs with a player already in the world.
	 *
	 * No local player here, and the split from FSessionFixture is deliberate: this one is handed to
	 * `FBrickTestWorldWrapper::BeforeBeginPlay`, which runs before the world has begun play at all,
	 * and attaching one there drags in a viewport-less `UGameViewportClient` and an `ensure` this
	 * project has been bitten by. The game-mode claims below read the session state and the build
	 * component, neither of which needs a local player.
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
 * The toolbar is the only door: every accepted click moves the state and the component, and every
 * refused click moves neither.
 *
 * The default mode is Destroy, not the model's own default, because `FSessionToolbarState::Mode`
 * defaults to Build and `Core/SessionToolbar.h` argues for it — a default-constructed session must
 * be the one that cannot destroy anything. The controller is a different question: twenty-eight of
 * the twenty-nine playable levels lay a structure and invite the player to pull it apart, and a
 * controller that opened them in Build mode would put a gold ghost over somebody else's wall and
 * swallow the first click. So the controller seeds Destroy — today's behaviour, unchanged for every
 * scenario level — and `GameModeOpensBuildModeOnThePlot` below is what puts the one build level into
 * Build mode, through the same single door.
 *
 * The refusals are the half that cannot be got right by accident. Three buttons are refused here,
 * each for a different reason:
 *
 *   - `Course down` at course 0 is drawn but greyed. The model already answers the state
 *     bit-for-bit unchanged; what this adds is that the component did not move either. A controller
 *     that pushed `SetCourse(state.Course)` unconditionally after the transition would pass every
 *     model test and still be correct here by luck, because the refused state's course is 0 — so
 *     the assertion that bites is the third `Course down`, taken after two accepted ones, where a
 *     controller decrementing its own copy would land on -1 while the component clamps to 0.
 *   - `Run structure` is not drawn at all in Build mode. The model's refusal is the FindByPredicate
 *     coming back null, and a controller that switched on the id before asking the model would run
 *     a solve on a build the player is still laying.
 *   - `Clear build` is drawn and greyed because nothing has been laid. Its side effect is
 *     cancel-then-begin, which changes the structure id — so the id being unchanged is what says
 *     the side effect did not run, a stronger reading than the piece count, which is zero either way.
 *
 * The accepted clicks are asserted in both currencies: the state is the presenter's record and the
 * component is what the world does. Asserting only the first passes against a controller that never
 * wired the component up at all; asserting only the second passes against one whose strip then greys
 * the wrong buttons. The piece click is the sharpest of the pair — the extent and the build plane are
 * derived by the component from the kind, so `(33.75, 5.125, 5)` and a plane of 20 cm on course 2 are
 * three facts one call has to get right.
 *
 * A world ticks for the component's structure and its ghost actor, but no test here ticks one.
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

	/* --- ONE: what a player opens a scenario level with ---------------------------------- */

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

	/* --- TWO: Build opens a build, and shows the cursor ----------------------------------- */

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

		/*
		 * AND A BUILD IS OPEN. Build mode with no structure behind it is a mode in which every click
		 * fails closed and nothing says why — the ghost would preview against an unknown id and
		 * ConfirmPlace would refuse every commit.
		 */
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

		/*
		 * THE CURSOR IS THE OTHER HALF OF "BUILD MODE IS USABLE". There is no aiming a ghost, and no
		 * pressing a chip, without a pointer. S6 makes it permanent — SetSessionControls raises it
		 * in BeginPlay and nothing lowers it — so this reads the same as it always did while meaning
		 * something weaker than it used to: the mode switch no longer has to raise it, it only has
		 * to leave it up. Section NINE is the half that bites.
		 */
		TestTrue(
			TEXT("Build mode must show the mouse cursor — there is no aiming a ghost with a camera "
				 "that follows every mouse movement"),
			Controller.bShowMouseCursor);
	}

	const int32 BuildStructureId = Build.GetStructureId();

	/* --- THREE: the piece click derives material, extent and plane ------------------------ */

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

	/* --- FOUR: Snap/Free rides through to the component ----------------------------------- */

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

	/* --- FIVE: the course stepper moves the build plane ----------------------------------- */

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

		/*
		 * The plane is a claim with two facts: a plate is still selected, so it is the plate's plane
		 * (2 * 7.5 + 5 = 20, not the brick's 18.25). Pushing the course without SetPieceKind's
		 * derivation would leave a 10 cm board buried half in the course below (see its header).
		 */
		TestEqual(
			FString::Printf(
				TEXT("and the plate on course 2 planes at 2 * 7.5 + 5 = 20 (NOT the brick's 18.25); it "
					 "is %g"),
				Build.BuildPlaneZCm),
			Build.BuildPlaneZCm, SessionPlatePlaneCourse2Cm);
	}

	/* --- SIX: down to the floor, and then REFUSED ----------------------------------------- */

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

	/* --- SEVEN: a button the strip does not draw at all ----------------------------------- */

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

	/* --- EIGHT: a command whose precondition is not met ----------------------------------- */

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

		/*
		 * The structure id is the assertion that bites: Clear's side effect is cancel-then-begin, so
		 * running it anyway leaves a different id, while the piece count (zero either way) says nothing.
		 */
		TestEqual(
			FString::Printf(
				TEXT("and above all leave the build it already had open: structure %d, not a fresh "
					 "one; the component holds %d"),
				BuildStructureId, Build.GetStructureId()),
			Build.GetStructureId(), BuildStructureId);
	}

	/* --- NINE: and the cursor does not go away when the mode does -------------------------- */

	{
		TestTrue(
			TEXT("the Destroy tab is always live"),
			Controller.OnToolbarButton(EToolbarButtonId::ModeDestroy));

		/*
		 * The cursor is the session's, not the mode's (SESSION_UI_DESIGN §d, S6).
		 *
		 * `OnToolbarButton(ModeDestroy)` used to hand controls back through the piece menu's own
		 * apply, so switching mode hid the pointer — with the toolbar always on screen that made it
		 * unclickable until a piece menu was first opened. Destroy hovers and inspects with the same
		 * pointer Build aims a ghost with, and the camera is a held right-drag in both, so the cursor
		 * cannot be something a mode raises and lowers; since S6 it is not — `SetSessionControls`
		 * raises it once in BeginPlay and no mode switch touches it. Same reason the mode must not
		 * change how the camera works: re-learning flying every switch would stop players switching.
		 */
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
 * The mode decides what a click is: in Build a ray lays a brick, in Destroy the same ray pulls one
 * out — and the build survives the round trip.
 *
 * One test, not three, because the product claim is a loop: lay something, switch mode, take it
 * apart, switch back and keep laying. Split by mode, every test would pass against a controller
 * that reset the build on every mode switch — the likeliest way to get this wrong, since
 * `CancelBuild` is right there and "leave Build mode" reads like a reason to call it. The id staying
 * unchanged across the round trip, and a third brick landing on the same structure the first two are
 * in, closes that.
 *
 * No piece menu in Build mode is asserted, not assumed: `InspectAlongRay` is wired to the same mouse
 * button as the Build click, so a controller that dispatched the preview by mode but left inspect on
 * the click would put a Delete menu over the brick just laid. Build asserts `IsPieceMenuShown()` is
 * false; Destroy asserts it is true — the same reading both ways is what makes either mean anything.
 *
 * The ghost is asserted on visibility, never position — position is already pinned to the centimetre
 * in `World.BuildMode.ComponentRayDrivesPreviewAndGhost`. New here is that a ray reaches it at all in
 * Build mode, and that leaving Build mode takes it off screen rather than leaving a gold brick
 * hanging over a wall being demolished.
 *
 * The numbers: the build plane is course 0's, 3.25 cm, so a ray straight down lands on it. The
 * first click has an empty structure under it, so the snap solver's only candidate is Free at the
 * picked point — (0, 0, 3.25) exactly, bottom face on the earth, grounded by the pose rule. The
 * second cursor is at x = 22.0; the same-course pose beside the seed is at 22.5 (21.5 brick + 1.0
 * joint), 0.5 cm away against 13.1 cm for the nearest next-course pose, so the bond wins and forms
 * one head joint. The third is one bay further at x = 45.0: 0 cm from the same-course pose beside
 * the second brick, 45 cm from anything the deleted first brick leaves behind.
 *
 * A world ticks for the spawns and for the Destroy ray's real line trace against real brick
 * collision, but no test here ticks one — nothing is about anything falling.
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

	/* --- ONE: pointing in Build mode drives the ghost ------------------------------------- */

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

	/* --- TWO: the primary click in Build mode LAYS A BRICK and opens no menu --------------- */

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

			/*
			 * Grounded by the pose, the only thing that makes the first brick a foundation: its
			 * bottom face is at Z = 0, within the 1 cm joint the rule allows.
			 */
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

	/* --- THREE: a second click, snapped onto the bond ------------------------------------- */

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

		/*
		 * One connection is the mechanism reading of "it bonded" — two bricks sitting beside each
		 * other with no joint between them look identical on screen but behave nothing like a wall.
		 */
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

	/* --- FOUR: the strip now knows there is something to command --------------------------- */

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

	/* --- FIVE: leaving Build takes the ghost off screen ------------------------------------ */

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

	/* --- SIX: pointing in Destroy mode calls out the brick under the cursor ---------------- */

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

		/*
		 * Pointing is not clicking — hover and inspect are separate seams so sweeping the cursor
		 * along a wall selects nothing.
		 */
		TestTrue(
			TEXT("and a hover must open no menu — pointing is not clicking"),
			!Controller.IsPieceMenuShown());
	}

	/* --- SEVEN: the primary click in Destroy mode puts that brick's menu up ---------------- */

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

	/* --- EIGHT: choosing it takes that brick and only that brick -------------------------- */

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

		/* Only that one — a delete that took the wall down would satisfy the rows above too. */
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

	/* --- NINE: and back to Build — the SAME build, still being laid ----------------------- */

	{
		TestTrue(
			TEXT("the Build tab is always live"),
			Controller.OnToolbarButton(EToolbarButtonId::ModeBuild));

		/*
		 * The id is the assertion this whole test is built around: `CancelBuild` is one call away
		 * and "leaving Build mode" reads like a reason to make it, which would hand the player a
		 * fresh empty plot every time they looked at a brick.
		 */
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
 * Run structure settles what the player built: the brick with nothing under it is released, and the
 * one on the ground is not.
 *
 * The floating brick is the whole fixture: `SolveAndPush` on a build that already stands releases
 * nothing, and "nothing happened" is exactly what a Run button wired to no solver produces. So the
 * build deliberately contains one piece that cannot stand — a Free placement six courses up with no
 * joints and no ground under it — which the solver must read Falling, and releasing it is the one
 * observable difference between a Run that ran and one that did not. The seed is not decoration
 * either: a Run implemented as "release everything" satisfies the floating brick's row perfectly, so
 * the grounded seed staying kinematic is what says the solver was consulted rather than the
 * structure being pushed wholesale.
 *
 * `IsReleased`, never displacement: nothing here ticks, so nothing has moved when the assertions
 * run, and even with a tick DESIGN §4 forbids reading displacement as evidence of a break.
 * `IsReleased` is the binding's own record of "this has been handed to Chaos", which is what Run does.
 *
 * The numbers: six course steps put the brick's build plane at 6 * 7.5 + 3.25 = 48.25 cm, so its
 * bottom face is 45 cm up — forty-five times the 1 cm joint the grounded rule allows, so it cannot
 * read grounded by rounding. It is laid at x = 100, far outside the 30 cm snap radius, so it forms
 * no joint even in Snap mode; Free makes it jointless for a second, independent reason. Both are
 * asserted as fixture preconditions, since a floating brick that turned out jointed or grounded
 * would stand and the test would go green over a dead Run button.
 *
 * A world ticks for the spawns and the release, but no test here ticks one.
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

	/* The grounded seed: a snapped brick on course 0, which must still be standing afterwards. */
	Controller.PrimaryAlongRay(SessionPointerRayStart(0.0), SessionPointerRayEnd(0.0));

	/* The floating brick: Free placement, six courses up, a hundred centimetres away. */
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

	/* --- THE FIXTURE PRECONDITIONS: one piece that must fall, one that must not ------------ */

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

	/* --- RUN, FROM DESTROY MODE, THROUGH THE ONE DOOR -------------------------------------- */

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
 * Clear build empties the plot and leaves a fresh one open — no brick standing, no binding orphaned,
 * and the player still in Build mode.
 *
 * Two currencies, again, because both are needed: `CancelBuild` destroys the structure and the
 * bricks it spawned. A clear that dropped the binding and left the bricks standing gives the player
 * a plot full of masonry nothing in the model knows about — colliders with no pieces behind them. A
 * clear that destroyed the bricks and kept the binding gives the opposite: a structure the solver
 * still reasons about with no bricks to show for it. So the world is counted (not one `ABrickActor`
 * that is not the ghost) and the id is read.
 *
 * "A fresh build is open" means a different id, not merely a valid one: `BeginBuild` spends an id on
 * an empty binding and the subsystem's ids are monotonic, so the new build cannot be the old one.
 * Asserting only `!= INDEX_NONE` would pass against a controller that cancelled nothing and kept the
 * same binding — precisely the bug where Clear appears to work and the next brick joins a wall that
 * is no longer on screen.
 *
 * The mode is unchanged: Clear is a command, not a mode — `SessionToolbarIsActive`'s default arm
 * already says a latched Clear reads as a mode the player is stuck in, and a Clear that dropped the
 * player back into Destroy would be the same confusion one layer up.
 *
 * A world ticks for the spawns and destroys, but no test here ticks one.
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

	/* --- THE CLEAR ------------------------------------------------------------------------ */

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

	/* --- AND A FRESH PLOT IS OPEN --------------------------------------------------------- */

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
 * Joining the build level puts the player in Build mode with a plot open — and joining any other
 * level does not.
 *
 * Both rows in one test because "the build level opens in Build mode" is satisfied by a controller
 * that opens every level in Build mode, which is the regression this pairing stops: twenty-eight of
 * the twenty-nine levels lay a structure for the player to pull apart, and a ghost over the middle
 * of one of those walls would swallow the first click on it. So the sandbox row is asserted in the
 * same test, from the same fixture, reading the same two fields.
 *
 * Run has something to act on a scenario level: `GetSessionStructureId` is what `Run structure`
 * solves, and its fallback is why a Destroy session on a scenario level is not inert — the player
 * has laid nothing, so the build component names no structure and the game mode's own wall is what
 * the button must reach. Asserting it equals `GetBuiltStructureId()` is what makes Run work on the
 * twenty-eight levels that come with a wall already built.
 *
 * On the build level the game mode builds nothing, so the fallback answers INDEX_NONE until the
 * player lays their first brick — correct, and why `Run structure` is greyed there until they do.
 * `GameModeOpensAnEmptyBuildSandbox` owns "nothing was laid"; this owns "the session opened in the
 * right mode".
 *
 * A world ticks with begin-play run under a URL, but no test here ticks one; the sandbox arm lays
 * the default wall, which is where its ~200 ms goes.
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

	/* --- ONE: ?Scenario=build opens Build mode with a plot open --------------------------- */

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

	/* --- TWO: ?Scenario=sandbox opens Destroy mode over the wall it built ------------------ */

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
 * The session's structure is the one with something live in it: a build whose only brick has been
 * deleted must not shadow the wall the level built.
 *
 * After the player lays one brick on a scenario level and then deletes it, `GetSessionStructureId`
 * must name the level's wall again — because the build now holds a tombstone and nothing else — and
 * the strip must therefore offer `Run structure` live.
 *
 * Two readings of "is there a structure" disagree: `GetSessionStructureId` prefers the player's
 * build "once there is something in it" and asks `NumPieces() > 0`; `RefreshSessionHasStructure`
 * asks the chosen structure for `NumLivePieces() > 0`, because RemovePiece tombstones rather than
 * compacting, so an emptied plot still answers a piece count. The two readings agree everywhere
 * except a build that has had pieces and has none left: the first picks the build (one tombstone),
 * the second reads zero live pieces off it, so `bHasStructure` goes false and both commands grey
 * themselves over a 1,220-brick wall standing in front of the player. The fix is one reading, used
 * twice.
 *
 * The fixture is a real scenario level because the bug needs both structures at once: a build that
 * is empty-but-not-absent, and a wall behind it for the fallback to reach. A bare world has no wall,
 * so the fallback answers INDEX_NONE and the two readings agree, asserting nothing. `?Scenario=sandbox`
 * is the same arm `GameModeOpensBuildModeOnThePlot` uses. The brick is laid thirty metres clear of
 * that wall because the Destroy click is a real line trace and a brick inside the wall's span could
 * put one of the level's own bricks under the cursor.
 *
 * The state is refreshed through a real door before it is read: the Destroy tab (always live, a
 * bitwise no-op on the state) is clicked before the flag is read, since `OnToolbarButton` refreshes
 * at the door so a command is never refused on a stale precondition. `GetSessionStructureId` itself
 * needs no such door. The refresh also happens on the delete path itself now
 * (`World.Session.DeleteRefreshesTheSessionFlag` covers that cold read with no intervening click);
 * the click here is belt and braces, kept because this test is about which structure the session
 * names, not when the flag is refreshed.
 *
 * A world ticks with begin-play run under a URL — the sandbox row lays its 1,220 bricks, which is
 * where the run time goes — and a real line trace against real collision, but no test here ticks
 * one; nothing is about anything moving.
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

	/* --- ONE: the level's own wall, which is what Run must go on reaching ------------------ */

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

	/* --- TWO: the player lays exactly one brick, well clear of it -------------------------- */

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

	/* --- THREE: and takes it straight back out again through the piece menu ---------------- */

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

	/* --- FOUR: the plot is now a tombstone, which is the whole point of the fixture --------- */

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

	/* --- FIVE: so the session's structure is the level's wall again ------------------------ */

	TestEqual(
		FString::Printf(
			TEXT("AN EMPTIED BUILD MUST NOT SHADOW THE LEVEL'S WALL. The player has deleted the one "
				 "brick they laid, so there is nothing of theirs to command and the session must name "
				 "the wall standing in front of them, structure %d. It names %d — the build, which "
				 "holds one tombstone and no live piece, because the choice is made on NumPieces "
				 "while whether there is anything to run is decided on NumLivePieces"),
			BuiltStructureId, Controller->GetSessionStructureId()),
		Controller->GetSessionStructureId(), BuiltStructureId);

	/* --- SIX: and the strip offers Run against it ------------------------------------------ */

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
 * Deleting the last piece refreshes the strip's one precondition: `bHasStructure` reads false
 * immediately, without waiting for the next toolbar click.
 *
 * After the player lays one brick and then deletes it through the piece menu, the session state
 * read cold — with no intervening `OnToolbarButton` — says there is nothing to command, so the
 * strip on screen greys `Clear build` and `Run structure` instead of offering them over an empty
 * plot.
 *
 * The read is cold on purpose: `OnToolbarButton` refreshes the precondition at the door, since a
 * command refused on a stale flag reads as a dropped click, but the delete path had no such door.
 * `ChoosePieceMenuRow` now refreshes both `RefreshSessionHasStructure` and `RefreshSessionToolbar` on
 * a committed action, and this is the test that holds it there — its discipline is the absence of
 * any toolbar click between the delete and the read; one click would make it pass without biting.
 * (`World.Session.SessionStructureIsTheLiveOne` reaches the same flag through a deliberate
 * Destroy-tab click instead.)
 *
 * A bare world, with no level wall behind the build: the sibling above needs a scenario level
 * because its bug is about which of two structures wins; this one must have only one, or
 * `GetSessionStructureId` would fall back to a wall and `bHasStructure` would be correctly true,
 * making the claim unfalsifiable. A bare `FBrickTestWorld` runs no game mode of this class, so the
 * fallback answers `INDEX_NONE` and "nothing to command" is simply the truth — asserted beside the
 * flag rather than assumed, so a failure says which of the two is wrong.
 *
 * What is asserted: the flag, the structure id it should derive from, and what the strip would draw
 * from it (`SessionToolbarButtons` asked for `RunStructure`). The last is the player-facing
 * statement and the reason the first two matter — a lit command over an empty plot is a button that
 * does nothing, which `Core/SessionToolbar.h` already argues is indistinguishable from a missed click.
 *
 * A world ticks for the spawns and for the Destroy ray's real line trace against real brick
 * collision, but no test here ticks one.
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

	/* --- ONE: one brick, and the session knows it has something to command ------------------ */

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

	/* --- TWO: and then the player takes it straight back out again ------------------------- */

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

	/*
	 * From here to the end of this test, nothing may touch the toolbar: `OnToolbarButton` refreshes
	 * the precondition at its door, so a single click of any button — even one the strip refuses —
	 * would make every claim below pass while the defect stands.
	 */

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

	/* --- THREE: the cold read ------------------------------------------------------------- */

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
 * CR-2b — the rotate chip turns the ghost's footprint, and a turned brick beside a laid one snaps to
 * the corner return.
 *
 * `OnToolbarButton(RotatePiece)` pushes the session's `bRotated` onto the build component
 * (`SetRotated`), which re-derives `CurrentExtentCm` as the palette's half extent with X and Y
 * swapped and leaves `BuildPlaneZCm` alone — so the very next preview beside an X-long brick is a
 * `BrickCornerReturn`, and the click that follows bonds it with full mortar.
 *
 * The extent is what is asserted, not a rotation, because nothing downstream of the toolbar knows
 * what an angle is: `FPieceBox` is an axis-aligned centre and half extent, the snap solver reads a
 * long axis off those numbers, and the joint inference classifies a contact from the two boxes and a
 * normal — so "rotated" is the swapped extent, with no second representation to check. A component
 * that stored a flag and went on previewing a 21.5 cm stretcher would satisfy any claim phrased as
 * "is it rotated"; the half extent is the only reading that cannot be satisfied by remembering the
 * click.
 *
 * The plane must not move: Z is untouched by a rotation about it, so a brick on course 0 is still
 * centred at 3.25 whichever way it lies. A `SetRotated` that re-derived the plane from the swapped
 * extent would put a rotated brick's centre at its own half width (5.125), burying every rotated
 * piece 1.875 cm into the earth with no course readout ever mentioning it. The plate is in the table
 * for the same reason: its half height (5.0) is not one of the two numbers that swap, so a plate
 * laid rotated is where a plane derived from the wrong axis becomes visible at all.
 *
 * The proof ends in a corner return rather than three numbers because the extents are the
 * mechanism; the corner return is what they are for. CR-2a taught the solver to offer a quoin when
 * two brick-sized boxes cross long axes, and `Core.BuildMode.CornerWallStands` proves that through
 * `PlacePiece` — but until this chip exists there is no way for a player to produce a crossed box at
 * all, so the whole corner vocabulary is unreachable from the game. The pose, joint count and
 * profile here are the same numbers `CornerWallStands` pins at its own step 3, measured off the seed
 * brick instead of brick 2: a return finishing flush with an X-long brick's -Y face, one joint, and
 * full `GeneralPurposeMortar` rather than the weak perpend a pre-CR-2a inference gave every vertical
 * face.
 *
 * The cursor is at Y = 5.0, not at the pose, because ranking is raw distance and four return poses
 * exist per crossed neighbour. The intended one is 0.625 cm from this cursor; its sibling at the
 * same end is 10.625 cm away, and the two off the seed's -X end are 33.75 cm away, outside the 30 cm
 * snap radius. Bed and head candidates need the same orientation and there are none, so the intended
 * pose wins outright rather than by a hair; the Free fallback — honouring the cursor verbatim at
 * (16.875, 5.0) — is appended last and cannot outrank a snap in Snap mode.
 *
 * A world ticks for the component's structure and its ghost actor, but no test here ticks one —
 * nothing here is about anything moving, and the placement is a graph mutation.
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

	/*
	 * THE FOUR FOOTPRINTS, SPELLED OUT RATHER THAN DERIVED FROM THE PALETTE. Asking
	 * BuildPieceHalfExtentCm and swapping its X and Y here would make this test agree with the
	 * palette however wrong it is, and the swap itself is the behaviour under test.
	 */
	const FVector BrickUprightHalfCm(10.75, 5.125, 3.25);
	const FVector BrickRotatedHalfCm(5.125, 10.75, 3.25);
	const FVector PlateRotatedHalfCm(5.125, 33.75, 5.0);

	/*
	 * THE CORNER RETURN'S NUMBERS, WORKED OFF THE SEED BRICK AT THE ORIGIN.
	 *
	 * The seed is X-long, half (10.75, 5.125, 3.25). A rotated brick returning off its +X end stands
	 * one joint clear of that end face: 10.75 + 1.0 + 5.125 = 16.875. Finishing FLUSH with the
	 * seed's -Y face (y = -5.125) puts its centre at -5.125 + 10.75 = 5.625. Course 0 leaves Z at
	 * 3.25. The quoin it forms is the seed's end face, 10.25 cm of width by 6.5 cm of course.
	 */
	const FVector CornerReturnCentreCm(16.875, 5.625, 3.25);
	constexpr double CornerCursorXCm = 16.875;
	constexpr double CornerCursorYCm = 5.0;
	constexpr double QuoinAreaSqCm = 66.625;

	/*
	 * A DOUBLE TOLERANCE, SPELLED OUT. Every number here is a sum of exact halves on the coordinating
	 * grid, so the comparison is effectively exact and the tolerance is only there to keep a
	 * floating-point equality honest. `KINDA_SMALL_NUMBER` is a FLOAT and makes TestEqual's double
	 * overload ambiguous, which is a compile error rather than a looser test.
	 */
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

	/* --- ONE: a session opens with an UPRIGHT brick --------------------------------------- */

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

	/* --- TWO: the click swaps X and Y, and leaves Z where it was --------------------------- */

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

	/* --- THREE: choosing another piece while rotated re-derives the SWAPPED extent ---------- */

	/*
	 * The plate is the case that tells a re-derivation from a remembered swap: `SetPieceKind` reads
	 * the palette afresh, so it has to honour a rotation chosen before it — a component that swapped
	 * the extent inside `SetRotated` alone would hand back an upright 67.5 cm board here and the
	 * ghost would silently un-rotate itself on a palette click. Its half height, 5.0 rather than
	 * 3.25, is what makes the plane assertion mean something: 5.0 is not one of the two numbers that
	 * swap, so a plane derived off the wrong axis reads 5.125 here, visibly not the plate's own.
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

	/* --- FOUR: the same chip turns it back -------------------------------------------------- */

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

	/* --- FIVE: an upright seed brick on the earth ------------------------------------------- */

	{
		const bool bPlaced = Controller.PrimaryAlongRay(
			SessionPointerRayStart(0.0), SessionPointerRayEnd(0.0));

		TestTrue(
			FString::Printf(
				TEXT("fixture: the seed brick must land at the origin; the click reported %d"),
				bPlaced ? 1 : 0),
			bPlaced);
	}

	/* --- SIX: rotated, the ghost beside it previews a CORNER RETURN -------------------------- */

	TestTrue(
		TEXT("rotate again, to lay the return"),
		Controller.OnToolbarButton(EToolbarButtonId::RotatePiece));

	const FVector CornerRayStart(CornerCursorXCm, CornerCursorYCm, SessionRayStartZCm);
	const FVector CornerRayEnd(CornerCursorXCm, CornerCursorYCm, SessionRayEndZCm);

	/* The player's own seam first — what a moving cursor does, putting the ghost on screen at the
	 * pose the click will take. */
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
	 * The pose is read back through the component's non-mutating query, the only way to read what
	 * the pointer just drove: `PointerAlongRay` returns nothing, and the ghost actor's transform is a
	 * spawn transform whose pivot is the brick's corner rather than its centre. `UpdatePreviewFromRay`
	 * is the very call `PointerAlongRay` makes, with the same arguments.
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

	/* --- SEVEN: the click commits it, and the quoin is FULL MORTAR --------------------------- */

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

			/*
			 * All five fields, because the mortar and its perpend sibling differ on two of them
			 * (cohesion 0.9 vs 0.2, tension 0.7 vs 0.1) and nothing else — exactly the pair this
			 * assertion has to tell apart. A one-field check would admit the weak perpend a
			 * pre-CR-2a inference gave every vertical face.
			 */
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

			/*
			 * And the normal is horizontal, which is what makes the profile claim mean anything: a
			 * mortar joint across a vertical normal is an ordinary bed and proves nothing about
			 * corners; |X| == 1 says this is the end face of the seed.
			 */
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
 * CR-2b (xiii) — a corner the player lays is judged by the LP, not silently demoted to the router.
 *
 * `UDestructionStructureSubsystem::BeginBuild` opens the player's build flagged three-dimensional,
 * so the Y-normal head joints a rotated leg forms are posed by the rigid-block bridge rather than
 * refused — and the whole build's break verdict on `Run structure` comes from the LP below the cap,
 * as it does for every other below-cap structure in the game.
 *
 * The defect, and why it is invisible: nothing in the session ever calls `SetThreeDimensional`.
 * Only the scenario builders do, and `AdoptLayout` carries their flag across
 * (`Core/StructureBinding.cpp` ~386) — a player's build is adopted from nothing and starts false.
 * The router does not care: `SolveLoads` reads normals directly and is dimension-agnostic, so a
 * corner wall routes its load down its beds and reads perfectly healthy. The one reader of the flag
 * is `RigidBlockOracle::BuildRigidBlockProblem`, and with it unset that function refuses the whole
 * problem the moment it meets a joint whose normal has a Y component. `FStructure::BreakByEquilibrium`
 * turns that refusal into `EEquilibriumGateDisposition::DeclinedToRouter` and says nothing, so one
 * rotated brick anywhere in a build moves the break authority for the entire build — straight legs
 * included — off the LP and onto `BreakByCapacitySweep`. The LP stands knot and opening arrangements
 * the router strands, fells the leaning-stack class the router holds, and is the only thing that
 * applies first-crack; none of that reaches a build with a corner in it. Nothing on screen says so —
 * the wall stands either way, the "wrong answer that looks plausible" DESIGN §5 names as this
 * codebase's recurring enemy.
 *
 * Two assertions, both needed. Mechanism: `IsThreeDimensional()` on the binding's structure,
 * asserted twice — once on the empty build the Build tab opens, once on the finished L. The empty
 * one is load-bearing: it forbids the cheap fix of flagging 3D when the first Y-normal joint forms,
 * which would be inference, ruled out by `FStructure::SetThreeDimensional`'s own contract (the E3
 * ruling, Structure.h ~490) — the intent to be 3D has to be stated, not guessed, or a cliff appears
 * mid-build where the authority deciding whether the wall stands changes as a brick lands. Outcome:
 * after `Run structure`, the LP's own per-joint readout is present (`GetConnectionReadout(k).bPresent`)
 * — the honest witness that the gate answered, since the cache is cleared every pass and refilled
 * only by an arm that reached a verdict, so absent means declined. `Released == 0` is asserted too
 * but proves nothing alone, since the router stands this wall perfectly well today, which is exactly
 * how the defect hides. Never displacement, in either direction: nothing here ticks, and DESIGN §4
 * forbids reading a distance as evidence of a break regardless.
 *
 * Why the fixture is six pieces and not three — the grounded-pair skip. The obvious fixture is the
 * two-piece corner plus one more Y-leg brick, forming a genuine Y-normal head joint, but it does not
 * work: the bridge drops any joint whose two pieces are both grounded before it looks at the normal
 * (`RigidBlockBridge.cpp` ~133). Every piece on course 0 is grounded, so an L laid entirely on the
 * earth presents the bridge with no Y-normal joint at all, is not refused, and answers today for a
 * reason that has nothing to do with the flag. So the Y leg is carried up a course: 5-4 is a head
 * joint between two pieces that reach the earth only through their beds, it is posed, and it is what
 * the 2D bridge refuses. The same skip is why the readout assertions run over posed joints only — a
 * skipped joint has no provenance entry and therefore no readout however the structure is flagged.
 *
 * The numbers, worked off the coordinating grid: the brick is 21.5 x 10.25 x 6.5 on a 1 cm joint, so
 * the grid is 22.5 x 11.25 x 7.5 and course n centres a brick at n * 7.5 + 3.25. The poses are
 * `Core.BuildMode.CornerWallStands`' own, measured off a one-brick X leg instead of three:
 *
 *   - the seed, upright at the origin, (0, 0, 3.25);
 *   - the return, rotated, one joint off the seed's +X end (10.75 + 1 + 5.125 = 16.875) and flush
 *     with its -Y face (-5.125 + 10.75 = 5.625), so the pair reads as an L;
 *   - the Y leg, same course, stepping 22.5 cm along Y: 28.125 and 50.625;
 *   - course 1, staggered half a pitch: 16.875 and 39.375, each bedded on the two below it.
 *
 * The cursors are offset deliberately, as the screenshot harness's table is: same-course bricks are
 * asked for 0.125 cm short of their pitch so the same-course pose beats the next-course one on raw
 * distance, and course-1 bricks are asked for at the running bond, where a zero offset cannot be
 * outranked. The return is asked for at (16.875, 5.0): the intended one of the four corner poses is
 * 0.625 cm away and its nearest sibling is 10.625 cm away.
 *
 * A world ticks for the binding's bricks and the ghost actor, but no test here ticks one — every
 * reading is a graph mechanism.
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

	/* The cursor for each of the six clicks, in order. See the header for every number. */
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
	 * Five posed, three skipped. The three earth-to-earth joints are the quoin (1-0) and the two
	 * course-0 heads (2-1, 3-2); the five the LP poses are the four beds (4-1, 4-2, 5-2, 5-3) and
	 * the course-1 head 5-4 — the one out-of-plane joint the 2D bridge has to meet. Pinned as counts
	 * so a fixture that quietly stopped forming one of them cannot make the readout sweep vacuous.
	 */
	constexpr int32 CornerPosedJoints = 5;
	constexpr int32 CornerSkippedJoints = 3;

	/** The six pieces' grounding, by handle: four on the earth, two reaching it through beds. */
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

	/* --- ONE: THE FLAG IS SET AT THE DOOR, ON A BUILD WITH NOTHING IN IT -------------------- */

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

	/* --- TWO: lay the L, through the player's own clicks ------------------------------------ */

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

	/* --- THREE: the fixture guards — this really is a corner, with a POSED Y-normal joint ---- */

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

	/* --- FOUR: THE MECHANISM — the finished corner build is 3D ------------------------------ */

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

	/* --- FIVE: THE OUTCOME — Run is answered by the LP, not by the router -------------------- */

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
 * CR-2b (xiii), the control — a straight build is flagged 3D too, and still reads the LP.
 *
 * The same `BeginBuild` flag is unconditional — a build with no rotated piece in it is opened 3D as
 * well — and posing a planar running-bond wall three-dimensionally does not cost it its LP answer.
 *
 * A separate test rather than a section, because it is the pair to `CornerBuildIsJudgedByTheLP` and
 * is where "unconditional" is pinned. The corner test alone is satisfied by a fix that flags 3D when
 * a rotated piece is placed, or when a Y-normal joint appears — both inferences the E3 ruling
 * forbids, and both a cliff mid-build. This wall contains no rotated piece and no Y-normal joint at
 * all, so the flag assertion here can only be met by stating the intent at the door.
 *
 * Its two halves arrive in different colours deliberately: the flag assertion is red today, exactly
 * as the corner's is; the readout assertion is green today, since this wall is 2D, the bridge poses
 * it happily and the LP answers. It is a regression net rather than a driver — the fix moves every
 * session build, including this one, onto the 3D pose, and a 3D pose that stopped answering for a
 * planar wall would take the LP off every straight wall a player lays. That assertion is proven to
 * bite by its twin in the corner test, the same line against the same accessor, which is red.
 *
 * The three numbers: course 0 at (0, 0, 3.25) and (22.5, 0, 3.25) — one brick plus one head joint
 * apart, asked for at x = 22.0 so the same-course pose wins by 0.5 cm against a next-course pose
 * 13.1 cm away — then course 1 at (11.25, 0, 10.75), asked for exactly at the running bond, which
 * beds on both below it. Three pieces, three joints: the course-0 head is earth-to-earth and skipped
 * by the bridge, so the two beds are the posed pair the readout is asserted over.
 *
 * A world ticks for the bricks; no test here ticks one.
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

	/* Course 1's plane for a brick: 1 * 7.5 + 3.25, and the running-bond stagger it lands on. */
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

	/* --- ONE: the flag at the door, with no rotation anywhere in this test ------------------- */

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

	/* --- TWO: two courses of running bond, laid through the player's clicks ------------------ */

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

	/* --- THREE: and Run still reads the LP ---------------------------------------------------- */

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
 * The planar pose, at the player's end — a straight build is flagged 3D and solved in 2D.
 *
 * A session build keeps the 3D flag `BeginBuild` states at the door, but the LP poses the cheapest
 * sound problem for what is actually in it — 2D for a straight wall, 3D for a build with a posed
 * out-of-plane joint — so a player who lays no corner never pays for one.
 *
 * A world test even though the rule is world-free: `Core.Oracle.PlanarProblemUnderThe3DFlagPosesIn2D`
 * pins the rule on the bridge's own output and needs no world. This one pins the wire — that the
 * pose a player's `Run structure` actually builds is the cheap one, through the same clicks and
 * binding they use. The measurement that forced the slice was taken here, not at the bridge: a cold
 * `Run structure` on a 100-brick / 261-joint straight session wall went from 2.5 s to 94 s, ~37x,
 * when `BeginBuild` began flagging every build 3D (CURRENT_STATE, corner entry (xiii)). No timing is
 * asserted — a wall-clock threshold on a shared machine flakes, and `OracleSweepFull` is where
 * solver cost is verified.
 *
 * The observable is an accessor, `FStructure::GetLastEquilibriumProblemDim()` — 2 or 3 for the
 * dimension the last equilibrium-gate pose was built in, INDEX_NONE before any. The problem struct
 * never leaves `BreakByEquilibrium`, so there is nothing else a world test can read; the
 * alternatives are both proxies that would pin the wrong thing (a solve time is a flake, and readout
 * values differing is the item-8 residue rather than the pose). `BreakByEquilibrium` stamps it from
 * `Problem.Dim` the moment the bridge accepts — on the pose, not the call — see the contract in
 * Structure.h.
 *
 * The flag is asserted alongside, and that pairing is the point: `IsThreeDimensional()` must still
 * be true on the straight build, since the cheap fix of un-flagging a build with no corner in it
 * would satisfy the dimension assertion and re-open the hole the E3 ruling closed — a build whose
 * authority changes as a rotated brick lands. The flag is the stated permission to pose 3D; the
 * bridge decides whether it needs to.
 *
 * Two builds, and the second is not optional: the straight wall is `StraightBuildIsJudgedByTheLP`'s
 * own three-piece fixture; the corner is `CornerBuildIsJudgedByTheLP`'s six-piece L, laid after a
 * `Clear build` so it gets a fresh binding with the same controller. Without the corner half,
 * "always pose 2D" passes — and that would pose the Y-facing head joint of every corner a player
 * lays onto an X-Z oracle that cannot express it, a plausible number with wrong statics rather than
 * a slow one.
 *
 * A world ticks for the binding's bricks and the ghost, but no test here ticks one. Every reading is
 * a posed dimension, a flag or a joint count — never a distance moved (DESIGN §4).
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

	/* The two dimensions, as the instrumentation reports them. Not an enum: see the accessor. */
	constexpr int32 PosedInTwoD = 2;
	constexpr int32 PosedInThreeD = 3;

	/* Course 1's running-bond stagger, the third click of the straight wall. */
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

	/* --- ONE: A STRAIGHT WALL, RUN --------------------------------------------------------- */

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

	/* --- TWO: THE CONTROL — A CORNER STILL RUNS IN 3D --------------------------------------- */

	{
		TestTrue(
			TEXT("fixture: back to Build to lay the second structure"),
			Controller.OnToolbarButton(EToolbarButtonId::ModeBuild));

		TestTrue(
			TEXT("fixture: Clear build must be live once something has been laid — it is how this "
				 "test gets a SECOND binding out of one controller"),
			Controller.OnToolbarButton(EToolbarButtonId::ClearBuild));

		const int32 CornerId = Build.GetStructureId();

		/*
		 * The six-piece L of `CornerBuildIsJudgedByTheLP`, cursor for cursor — see that test's
		 * header for where the numbers come from. Its copy of the table is left where it is
		 * deliberately: that test pins the flag and this one pins the pose, and a shared table
		 * would make one fixture's drift silently move both claims.
		 */
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

		/* Clear does not reset the stepper, and the L starts on the earth. */
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
 * Cursor-driven ghost — a toolbar click moves the ghost at once, with no second pointer event.
 *
 * With a preview already held from a pointer move, `Rotate`, a piece chip, `Free` and `Course up`
 * each re-drive that preview through the build component, so the ghost on screen shows the new
 * setting immediately rather than at the player's next mouse movement (the owner's playtest,
 * 2026-09-16: "it should show where the brick is going to go without clicking anything").
 *
 * A controller test as well as a component one: `World.BuildMode.SettingsChangeRefreshesTheHeldPreview`
 * pins the component's own doors. What that cannot see is the wiring — `OnToolbarButton` writes
 * `PlacementMode` as a bare field today, so a component that refreshed inside every setter would
 * still leave the ghost stale for the Snap/Free pair, since the click would never reach a setter at
 * all. This test drives the very door the strip's chips call, so it fails for the pair the
 * controller forgot as readily as for a setter that forgot to refresh.
 *
 * What is asserted is the ghost actor's world bounds, and nothing else — the thing the player is
 * complaining about, and the only pivot-agnostic reading of where an `ABrickActor` is drawn. Its
 * size carries the piece kind and the rotation (nothing downstream knows what an angle is: "rotated"
 * is the swapped half extent), and its centre carries the pose. Never a displacement — nothing is
 * released and nothing ticks.
 *
 * A brick is 21.5 x 10.25 x 6.5 on 1 cm joints, so the grid is 22.5 x 11.25 x 7.5 and a brick's half
 * height is 3.25: course 0 rests it at 3.25, course 1 at 10.75. The seed is laid at the origin,
 * X-long; the cursor ray is vertical at (11.25, 3.0), so it meets the course-0 plane at
 * (11.25, 3.0, 3.25) and the running-bond next-course pose (11.25, 0, 10.75) is the nearest snap
 * (8.08 cm, against 11.64 cm for the same-course pose beside the seed). Y = 3.0 is off-grid on
 * purpose: it keeps the two corner-return poses a rotated brick could take from being exactly
 * equidistant, so the rotated leg reads one well-separated answer rather than a tie broken by
 * emission order.
 *
 * The Free legs carry the course, and they have to: a snapped pose is decided by the neighbours, so
 * a course change cannot be read through it at all, while in Free placement the pose is the cursor,
 * so `Course up` must lift the ghost by exactly one course of 7.5 cm — 3.25 to 10.75 — an exact
 * reading of "the refreshed cursor sits on the current build plane".
 *
 * Red today: no settings click re-drives the preview, so the ghost keeps the footprint and pose it
 * had at the last pointer event.
 *
 * A world ticks for the component's structure and its ghost actor; no test here ticks one.
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

	/* The three footprints, spelled out rather than asked of the palette. */
	const FVector UprightBrickSizeCm(21.5, 10.25, 6.5);
	const FVector RotatedBrickSizeCm(10.25, 21.5, 6.5);
	const FVector UprightPlateSizeCm(67.5, 10.25, 10.0);

	/* Where the cursor points, and the three poses the ghost must take there. */
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

	/* Reading the ghost: bounds bNonColliding, because the ghost's collision is disabled. */
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

	/* --- ONE: a seed brick, then a pointer move that HOLDS a preview ------------------------- */

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

	/* --- TWO: the Rotate chip swaps the ghost's footprint where it stands -------------------- */

	/*
	 * The footprint, not the pose: a rotated brick beside an X-long one takes a corner return, and
	 * which of the four the solver ranks first is the snap solver's business — what the chip owes
	 * the player is that the thing on screen is the piece they just chose. The pose is logged for
	 * the reader rather than asserted.
	 */
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

	/* --- THREE: a piece chip re-draws it as the new piece ------------------------------------ */

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

	/* --- FOUR: the Free chip drops the ghost onto the cursor --------------------------------- */

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

	/* --- FIVE: and Course up lifts it by exactly one course ---------------------------------- */

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

	/* No settings click may COMMIT anything: the plot still holds the one seed brick. */
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
 * Cursor-driven ghost — the per-tick cursor refresh drives the ghost from a ray, in Build mode only.
 *
 * `RefreshBuildPreviewFromRay` is the half of the per-tick cursor refresh that a test can reach: in
 * Build mode it previews along the given ray exactly as a pointer move does and reports that a ghost
 * is up; in Destroy mode it is a no-op that shows nothing and reports false.
 *
 * The seam is a ray, not a cursor, because the tick's real first step is
 * `DeprojectMousePositionToWorld`, which needs a viewport and is untestable by construction — the
 * same inch `OnHoverPiece` and `OnInspectPiece` are already kept down to. So the deprojection stays
 * in `RefreshBuildPreviewFromCursor` (and its use from `PlayerTick`, which needs the owner's
 * playtest), and everything that can be wrong in a way a player would notice — which mode this is
 * allowed to run in, whether the ghost ends up where the ray points, whether it shows at all — lives
 * behind this call, which needs only a world.
 *
 * The Destroy leg is the one that cannot be got right by accident: a tick handler is the easiest
 * place in this controller to leak a mode, and a refresh that ran regardless would put a gold ghost
 * over the wall the player is demolishing every frame, and re-arm a preview a stray confirm could
 * commit — precisely the ghost `OnToolbarButton(ModeDestroy)` hides on the way in.
 *
 * The look-chord guard is not pinned here, deliberately: "`IA_LookModifier` is held" is only
 * readable through the Enhanced Input local-player subsystem with injected input, an order of
 * magnitude more fixture than the claim is worth and nothing in this suite does today; it is left to
 * the owner's playtest.
 *
 * The numbers are the ones the file's other build tests use: a seed brick at the origin on course 0
 * (centre Z 3.25), a vertical ray at (11.25, 3.0) meeting the course-0 plane at (11.25, 3, 3.25),
 * and the running-bond next-course pose (11.25, 0, 10.75) as the nearest snap. Assertions are on the
 * returned bool, the ghost's bounds and its visibility, and the binding's piece count — never a
 * displacement.
 *
 * Red today: `RefreshBuildPreviewFromRay` does not exist.
 *
 * A world ticks for the structure and the ghost actor, but no test here ticks one — that is the
 * point of the seam: the tick's own call is what the playtest checks.
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

	/* Straight down through (11.25, 3.0), so the plane hit is that point whatever the course. */
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

	/* --- ONE: a seed brick to snap against --------------------------------------------------- */

	{
		const bool bPlaced = Controller.PrimaryAlongRay(
			SessionPointerRayStart(0.0), SessionPointerRayEnd(0.0));

		TestTrue(
			FString::Printf(TEXT("fixture: the seed brick must land at the origin; the click "
								 "reported %d"),
				bPlaced ? 1 : 0),
			bPlaced);
	}

	/* --- TWO: in Build mode the refresh puts the ghost where the ray points ------------------ */

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

	/* --- THREE: in Destroy mode it does nothing at all --------------------------------------- */

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
 * Cursor-driven ghost, the hole in it — a click must re-drive the ghost along the same ray, with no
 * second pointer event.
 *
 * After a click places a piece, the ghost must immediately show where the next one would go along
 * that same ray — visible, standing at the pose a fresh preview at that ray answers, out of the
 * brick just laid, and held, so the next confirm lays the piece the player can already see.
 *
 * This is the gap, not a restatement of its two siblings: `CursorRefreshDrivesTheGhostFromARay` pins
 * the per-tick refresh and `ToolbarChangesMoveTheGhostAtOnce` pins the settings doors. Between them
 * sits the one moment neither covers — the click itself, with a still mouse. `PrimaryAlongRay`
 * re-previews before `ConfirmPlace` and never after; `ConfirmPlace` spends the held preview but
 * leaves the ghost actor standing, unhidden, exactly where the brick it just committed now is; and
 * the per-tick refresh is throttled on the cursor's pixel position, so it skips every frame until the
 * pointer moves. The result the owner sees is a gold ghost z-fighting the red brick inside it until
 * they jog the mouse — the moment "show where the brick is going to go" matters most, since laying a
 * course is a sequence of clicks without much mouse between them. It cannot be reached through the
 * tick, which needs a viewport, so it is driven through the same ray seam: refresh at R, click at R,
 * then assert with no further call.
 *
 * What is asserted, and why each is needed. All four claims are mechanism readings — the ghost
 * actor's world bounds, its hidden flag, the binding's piece count, and the committed piece's own
 * box centre. Never a displacement; nothing is released and nothing ticks.
 *
 *   - Visible. A cheap fix that hid the ghost on commit would answer the z-fight and leave the
 *     player with no preview at all until they moved the mouse, the same complaint.
 *   - Not the placed piece's centre. This is the z-fight itself, read as a mechanism rather than a
 *     distance moved: the ghost is not allowed to be standing inside the brick just laid.
 *   - Equal to a fresh ray preview at the same R, taken after the ghost's pose is snapshotted so the
 *     comparison is not circular — the snapshot is what production left behind, the oracle is what
 *     production, asked again, says the answer is. "Not the placed centre" alone would pass against
 *     a ghost parked at any arbitrary pose.
 *   - Held, not merely moved. `RefreshPreview()` returns whether a valid preview is held and refuses
 *     (false, touching nothing) when one is not, so it is the direct reading of the flag
 *     `ConfirmPlace` clears. A fix that only teleported the ghost actor would leave the next
 *     `ConfirmPlace` failing closed with a ghost on screen promising a brick, worse than the bug.
 *     Section SIX then spends it: a second click at the same R must lay a second piece at the pose
 *     the ghost had been showing all along.
 *   - The re-preview must not commit. A preview is a question, so the count is asserted at 2 before
 *     anything else in section FOUR — a re-drive implemented as "place and undo" is caught here
 *     rather than three sections later.
 *
 * The numbers, worked through. The file's usual grid: a brick is 21.5 x 10.25 x 6.5 on 1 cm joints,
 * so the coordinating grid is 22.5 x 11.25 x 7.5 and course 0 rests a brick at Z 3.25, course 1 at
 * 10.75. The seed goes at the origin. R is the vertical ray through (11.25, 3.0), meeting the
 * course-0 plane at (11.25, 3, 3.25). Before the click the nearest snap is the running-bond
 * next-course pose (11.25, 0, 10.75), 8.08 cm from that point against 11.64 cm for the same-course
 * pose beside the seed, and the click commits it. After the click that pose is occupied by the brick
 * that now stands there, so the solver drops it and the ranking's runner-up wins: the same-course
 * running-bond pose beside the seed, (22.5, 0, 3.25), at 11.64 cm — the other live poses are far
 * behind it (19.0 cm to 33.9 cm), so the answer is not a near-tie that emission order could flip.
 * That 11.25 cm gap in X between the ghost and the placed brick is comfortably outside the 0.05 cm
 * bounds tolerance, so "not inside the brick just laid" does not rest on float noise.
 *
 * Red today: nothing re-previews after the commit, so the ghost is left standing at the placed
 * centre (11.25, 0, 10.75) with no preview held.
 *
 * A world ticks for the component's structure and its ghost actor, but no test here ticks one — the
 * tick's own call is what the owner's playtest checks.
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

	/* R: straight down through (11.25, 3.0), so the plane hit is that point whatever the course. */
	constexpr double CursorXCm = 11.25;
	constexpr double CursorYCm = 3.0;

	const FVector CursorRayOriginCm(CursorXCm, CursorYCm, SessionRayStartZCm);
	const FVector CursorRayDirection(0.0, 0.0, -1.0);

	/* What the click commits, and where the ghost must stand once it has. */
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

	/* Reading the ghost: bounds bNonColliding, because the ghost's collision is disabled. */
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

	/* --- ONE: a seed brick to snap against --------------------------------------------------- */

	{
		const bool bPlaced = Controller.PrimaryAlongRay(
			SessionPointerRayStart(0.0), SessionPointerRayEnd(0.0));

		TestTrue(
			FString::Printf(TEXT("fixture: the seed brick must land at the origin; the click "
								 "reported %d"),
				bPlaced ? 1 : 0),
			bPlaced);
	}

	/* --- TWO: the cursor refresh at R puts a ghost up, exactly as the tick would --------------- */

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

	/* --- THREE: the click commits that pose --------------------------------------------------- */

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

	/* --- FOUR: and with NO further call the ghost must have moved on --------------------------- */

	FVector GhostCentreAfterClickCm = FVector::ZeroVector;

	{
		/* A preview is a question. Asserted FIRST, so a re-drive that placed and undid is caught. */
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

		/*
		 * And the preview must be held, not just the actor moved: `RefreshPreview` reports whether a
		 * valid preview is held and refuses (false, touching nothing) when one is not, so it is the
		 * direct reading of the flag `ConfirmPlace` clears. A ghost moved without one is a promise
		 * the next click cannot keep — the confirm would fail closed with a brick on screen.
		 */
		TestTrue(
			TEXT("AND A VALID PREVIEW MUST BE HELD: the ghost showing the next pose is only honest if "
				 "a confirm would commit THAT pose. ConfirmPlace spends the held preview and nothing "
				 "re-arms it, so today there is a ghost on screen and no preview behind it"),
			Build.RefreshPreview());
	}

	/* --- FIVE: the oracle — what a fresh preview at the SAME ray says, asked afterwards -------- */

	/*
	 * Snapshot first, oracle second, so the comparison is not circular: section FOUR read what
	 * production left behind, this asks production, on the unchanged binding, what the answer at R
	 * actually is. The expected value is also derived by hand in the header and pinned below, so an
	 * oracle that agreed with a wrong ghost would still be caught.
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

	/* --- SIX: and the next click lays a SECOND piece at the pose the ghost was showing --------- */

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
