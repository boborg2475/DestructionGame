// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

#include "Core/PieceMenu.h"
#include "Core/SessionToolbar.h"
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
 * SESSION S4/S5 — THE CONTROLLER IS THE SESSION: THE TOOLBAR IS THE ONLY DOOR INTO ITS STATE, AND
 * THE MODE DECIDES WHAT A RAY DOES.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * `ADestructionGamePlayerController` holds one `FSessionToolbarState`, changes it ONLY through
 * `OnToolbarButton` (which pushes the change onto its own `UBuildModeComponent` and refuses
 * whatever the model refuses, side effects and all), and dispatches a world ray by mode — Build
 * previews and places, Destroy hovers and inspects.
 *
 * =====================================================================================
 * WHY THE DOOR IS ONE FUNCTION AND WHY THAT IS THE THING UNDER TEST
 * =====================================================================================
 *
 * `Core/SessionToolbar.h` already owns which buttons exist, which are greyed, and what one click
 * does to the state — `ApplyToolbarButton` is pure and `Core.SessionToolbar.*` sweeps it. What no
 * model can own is the SIDE EFFECTS: a click that moves the course must move the component's build
 * plane, and a click the model REFUSES must move nothing at all. Those two are the same bug seen
 * from either end — a controller that applied the transition and then ran its side effect
 * unconditionally would have a greyed `Course down` quietly lowering the build plane below the
 * earth, with the readout still saying course 0. CURRENT_STATE records the component's kind, course
 * and placement as a PARALLEL COPY of three state fields for exactly this reason; this file is what
 * pins who owns them.
 *
 * =====================================================================================
 * WHAT IS ASSERTED, AND WHY IT IS NEVER DISPLACEMENT
 * =====================================================================================
 *
 * Every claim here is a MECHANISM reading: the returned bool, the state's own fields, the
 * component's structure id / piece kind / course / build plane, the binding's piece count and
 * connection count, `IsPieceMenuShown`, a brick's `GetHighlight`, and `IsReleased`. Nothing is read
 * as a distance moved — DESIGN §4 is explicit, and it bites hardest here: the Free brick in
 * `RunStructureSettlesTheBuild` is released 45 cm above the floor and would be measured falling by
 * a physics tick nobody runs. `IsReleased` is the answer to "did Run do its job"; where the brick
 * ends up is Chaos's business.
 *
 * NEEDS A TICKING WORLD: a WORLD, yes — the subsystem spawns real `ABrickActor`s, the ghost is an
 * actor, and the Destroy ray is a real line trace against real collision. None of these tests ticks
 * one; nothing here is about anything moving.
 *
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges many
 * files into one. See CURRENT_STATE.md; the `using namespace` lives inside each RunTest body for
 * the same reason.
 */
namespace SessionControllerTestSupport
{
	using namespace DestructionSession;

	/*
	 * THE RESTS-ON-THE-GROUND ARITHMETIC, SPELLED OUT RATHER THAN IMPORTED (DESIGN §8, 2026-09-15).
	 *
	 * A brick is 21.5 x 10.25 x 6.5 cm on a 1 cm joint, so the coordinating grid is 22.5 x 11.25 x
	 * 7.5 and the brick's own half height is 3.25. Course 0 therefore centres a brick at 3.25 with
	 * its underside ON the earth; course n centres it at n * 7.5 + 3.25. A timber plate is 5.0 cm
	 * half-height, so the SAME course puts it at n * 7.5 + 5.0 — which is the whole reason the plane
	 * is derived from the piece as well as from the course.
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
	 * Whether two states are the SAME state, field for field.
	 *
	 * FIELD BY FIELD RATHER THAN memcmp, because a struct with a bool in it carries padding and two
	 * states that differ only in their padding are the same state. The model's own refusal contract
	 * is a BITWISE no-op, and this is the honest reading of it.
	 */
	bool SessionSameState(const FSessionToolbarState& A, const FSessionToolbarState& B)
	{
		return A.Mode == B.Mode
			&& A.Piece == B.Piece
			&& A.Placement == B.Placement
			&& A.Course == B.Course
			&& A.bHasStructure == B.bHasStructure;
	}

	/**
	 * Whether the strip this state draws offers this button LIVE.
	 *
	 * ASKED OF THE PRODUCTION MODEL rather than re-decided here, which is the point: the claim is
	 * that the CONTROLLER's state is one the strip can grey correctly, not that this file agrees
	 * with the model about greying. A button the strip does not draw at all answers false.
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
	 * A controller in the world with a REAL ULocalPlayer, and its build component, or nulls.
	 *
	 * THE LOCAL PLAYER IS NOT DECORATION. `bShowMouseCursor` is a plain field and would flip without
	 * one, but `SetSessionControls` also sets the input mode and the session's mapping contexts are
	 * applied through the Enhanced Input LOCAL PLAYER subsystem — and a controller with no local
	 * player has none, so the engine never runs `SetupInputComponent` for it either. A fixture
	 * without one would assert the cursor half of the session's controls while the input half
	 * silently failed closed.
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
	 * NO LOCAL PLAYER HERE, and the split from FSessionFixture is deliberate: this one is handed to
	 * `FBrickTestWorldWrapper::BeforeBeginPlay`, which runs before the world has begun play at all,
	 * and `ScenarioLevelTestSupport` already records why attaching one there drags in a viewport-less
	 * `UGameViewportClient` and an `ensure` this project has been bitten by. The game-mode claims
	 * below read the session state and the build component, neither of which needs a local player.
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
 * THE TOOLBAR IS THE ONLY DOOR: EVERY ACCEPTED CLICK MOVES THE STATE **AND** THE COMPONENT, AND
 * EVERY REFUSED CLICK MOVES NEITHER.
 *
 * =====================================================================================
 * WHY THE DEFAULT MODE IS DESTROY AND NOT THE MODEL'S OWN DEFAULT
 * =====================================================================================
 *
 * `FSessionToolbarState::Mode` defaults to Build, and `Core/SessionToolbar.h` argues for it: a
 * default-constructed session must be the one that cannot destroy anything. The CONTROLLER is a
 * different question. Twenty-eight of the twenty-nine playable levels lay a structure and invite the
 * player to pull it apart, and a controller that opened them in Build mode would put a gold ghost
 * over somebody else's wall and swallow the first click. So the controller seeds Destroy — today's
 * behaviour, unchanged for every scenario level — and `GameModeOpensBuildModeOnThePlot` below is
 * what puts the ONE build level into Build mode, through the same single door.
 *
 * =====================================================================================
 * THE REFUSALS ARE THE HALF THAT CANNOT BE GOT RIGHT BY ACCIDENT
 * =====================================================================================
 *
 * Three buttons are refused here and each is refused for a different reason:
 *
 *   - `Course down` at course 0 is DRAWN but GREYED. The model already answers the state
 *     bit-for-bit unchanged; what this adds is that the COMPONENT did not move either. A controller
 *     that pushed `SetCourse(state.Course)` unconditionally after the transition would pass every
 *     model test and still be correct here by luck, because the refused state's course IS 0 — so
 *     the assertion that bites is the third `Course down`, taken after two accepted ones, where a
 *     controller decrementing its own copy would land on -1 and the component would clamp to 0
 *     while the state read -1.
 *   - `Run structure` is NOT DRAWN AT ALL in Build mode. The model's refusal is the FindByPredicate
 *     coming back null, and a controller that switched on the id before asking the model would run
 *     a solve on a build the player is still laying.
 *   - `Clear build` is DRAWN and GREYED because nothing has been laid. Its side effect is
 *     cancel-then-begin, which changes the structure id — so the id being UNCHANGED is what says
 *     the side effect did not run, and it is a stronger reading than the piece count, which is zero
 *     either way.
 *
 * =====================================================================================
 * AND THE ACCEPTED CLICKS ARE ASSERTED IN BOTH CURRENCIES
 * =====================================================================================
 *
 * The state is the presenter's record and the component is what the world does. Asserting only the
 * first passes against a controller that never wired the component up at all; asserting only the
 * second passes against one whose strip then greys the wrong buttons. The piece click is the
 * sharpest of the pair — the extent and the build plane are DERIVED by the component from the kind,
 * so `(33.75, 5.125, 5)` and a plane of 20 cm on course 2 are three facts one call has to get right.
 *
 * NEEDS A TICKING WORLD: a world for the component's structure and its ghost actor, but it never
 * ticks one.
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
		 * THE PLANE IS THE CLAIM WITH TWO FACTS IN IT. A plate is still selected, so the plane is the
		 * plate's — 2 * 7.5 + 5 = 20, not the brick's 18.25. A controller that pushed the course but
		 * not through SetPieceKind's derivation would leave a 10 cm board centred at a brick's height
		 * with half of it buried in the course below, which is the failure SetPieceKind's own header
		 * describes.
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
		 * THE STRUCTURE ID IS THE ASSERTION THAT BITES. Clear's side effect is cancel-then-begin, so
		 * a controller that ran it anyway would leave a DIFFERENT id behind — while the piece count,
		 * zero either way, would say nothing at all.
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
		 * THE CURSOR IS THE SESSION'S, NOT THE MODE'S (SESSION_UI_DESIGN §d, S6).
		 *
		 * `OnToolbarButton(ModeDestroy)` USED TO hand the controls back through the piece menu's own
		 * apply, so switching mode HID the pointer — and with the toolbar always on screen that was
		 * a strip a human could not click until they first opened a piece menu, which is not a
		 * toolbar. The strip is up in both modes, Destroy hovers and inspects with the same pointer
		 * Build aims a ghost with, and the camera is a held right-drag in both; so the cursor cannot
		 * be something a mode raises and lowers, and since S6 it is not — `SetSessionControls` raises
		 * it once in BeginPlay and no mode switch touches it.
		 *
		 * THE MODE MUST NOT CHANGE HOW THE CAMERA WORKS, which is the same sentence from the other
		 * end: a player who has to re-learn flying twice a minute is a player who stops switching
		 * mode.
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
 * THE MODE DECIDES WHAT A CLICK IS: IN BUILD A RAY LAYS A BRICK, IN DESTROY THE SAME RAY PULLS ONE
 * OUT — AND THE BUILD SURVIVES THE ROUND TRIP.
 *
 * =====================================================================================
 * WHY THIS IS ONE TEST AND NOT THREE
 * =====================================================================================
 *
 * The whole product claim of the session is a LOOP: lay something, switch mode, take it apart,
 * switch back and keep laying. Split into three tests it would be three claims about one mode each,
 * and every one of them would pass against a controller that reset the build on every mode switch —
 * which is the single most likely way to get this wrong, because `CancelBuild` is right there and
 * "leave Build mode" reads like a reason to call it. The id being UNCHANGED across the round trip,
 * and a third brick landing on the structure the first two are in, is what closes that.
 *
 * =====================================================================================
 * NO PIECE MENU IN BUILD MODE, ASSERTED RATHER THAN ASSUMED
 * =====================================================================================
 *
 * `InspectAlongRay` is today's click and it is wired to the same mouse button. A controller that
 * dispatched by mode for the PREVIEW but left the inspect on the click would put a Delete menu up
 * over the brick the player just laid, with the cursor already captured. So the Build clicks assert
 * `IsPieceMenuShown()` is false, and the Destroy click asserts it is true — the same reading, both
 * ways round, which is what makes either of them mean anything.
 *
 * =====================================================================================
 * THE GHOST IS ASSERTED ON VISIBILITY, NEVER ON POSITION
 * =====================================================================================
 *
 * Where the ghost sits is `World.BuildMode.ComponentRayDrivesPreviewAndGhost`'s claim and it is
 * already pinned to the centimetre there. What is NEW here is that a ray reaches it at all in Build
 * mode and that leaving Build mode takes it off screen — a gold brick left hanging in mid-air over a
 * wall the player is now demolishing. Hidden/visible is binary and immune to every tolerance.
 *
 * =====================================================================================
 * THE NUMBERS
 * =====================================================================================
 *
 * The build plane is course 0's, 3.25 cm, so a ray straight down lands on it. The first click has an
 * empty structure under it, so the snap solver's only candidate is Free at the picked point —
 * (0, 0, 3.25) exactly, bottom face on the earth, therefore GROUNDED by the pose rule. The second
 * cursor is at x = 22.0; the same-course pose beside the seed is at 22.5 (one brick of 21.5 plus one
 * joint of 1.0), which is 0.5 cm away against 13.1 cm for the nearest next-course pose — so the bond
 * takes it and forms exactly one head joint. The third goes one bay further along at x = 45.0, which
 * is 0 cm from the same-course pose beside the SECOND brick and 45 cm from anything the deleted
 * first brick leaves behind.
 *
 * NEEDS A TICKING WORLD: a world for the spawns and for the Destroy ray's real line trace against
 * real brick collision, but it never ticks one. Nothing here is about anything falling.
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
			 * GROUNDED BY THE POSE, WHICH IS THE ONLY THING THAT MAKES THE FIRST BRICK A FOUNDATION.
			 * Its bottom face is at Z = 0, within the 1 cm joint the rule allows.
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
		 * ONE CONNECTION IS THE MECHANISM READING OF "IT BONDED". Two bricks sitting beside each
		 * other with no joint between them look identical on screen and behave nothing like a wall.
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
		 * AND POINTING IS NOT CLICKING. The whole reason hover and inspect are separate seams is that
		 * sweeping the cursor along a wall must not select anything.
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

		/* AND ONLY THAT ONE. A delete that took the wall down satisfies the rows above perfectly. */
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
		 * THE ID IS THE ASSERTION THIS WHOLE TEST IS BUILT AROUND. `CancelBuild` is one call away and
		 * "leaving Build mode" reads like a reason to make it; a controller that did would hand the
		 * player a fresh empty plot every time they looked at a brick.
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
 * RUN STRUCTURE SETTLES WHAT THE PLAYER BUILT: THE BRICK WITH NOTHING UNDER IT IS RELEASED, AND THE
 * ONE ON THE GROUND IS NOT.
 *
 * =====================================================================================
 * WHY THE FLOATING BRICK IS THE WHOLE FIXTURE
 * =====================================================================================
 *
 * `SolveAndPush` on a build that already stands releases nothing, and "nothing happened" is exactly
 * what a Run button wired to no solver at all produces. So the build deliberately contains one piece
 * that CANNOT stand: a Free placement six courses up with no joints and no ground under it. That is
 * a piece the solver must read Falling, and releasing it is the one observable difference between a
 * Run that ran and a Run that did not.
 *
 * THE SEED IS THE OTHER HALF, AND IT IS NOT DECORATION. A Run implemented as "release everything"
 * satisfies the floating brick's row perfectly. The grounded seed staying kinematic is what says the
 * solver was consulted rather than the structure being pushed wholesale.
 *
 * =====================================================================================
 * IsReleased, NEVER DISPLACEMENT
 * =====================================================================================
 *
 * Nothing here ticks, so nothing has moved a millimetre when the assertions run — and even with a
 * tick, DESIGN §4 forbids reading displacement as evidence of a break. `IsReleased` is the binding's
 * own record of "this has been handed to Chaos", which is precisely what Run does.
 *
 * =====================================================================================
 * THE NUMBERS
 * =====================================================================================
 *
 * Six course steps put the brick's build plane at 6 * 7.5 + 3.25 = 48.25 cm, so its bottom face is
 * 45 cm up — forty-five times the 1 cm joint the grounded rule allows, so it cannot read grounded by
 * rounding. It is laid at x = 100, which is 100 cm from the seed and so far outside the 30 cm snap
 * radius that it would form no joint even in Snap mode; Free makes it jointless for a second,
 * independent reason. Both are asserted as fixture preconditions, because a floating brick that
 * turned out to be jointed or grounded would stand, and the test would go green over a dead Run
 * button.
 *
 * NEEDS A TICKING WORLD: a world for the spawns and the release, but it never ticks one.
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

	/* THE GROUNDED SEED: a snapped brick on course 0, which must still be standing afterwards. */
	Controller.PrimaryAlongRay(SessionPointerRayStart(0.0), SessionPointerRayEnd(0.0));

	/* THE FLOATING BRICK: Free placement, six courses up, a hundred centimetres away. */
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
 * CLEAR BUILD EMPTIES THE PLOT AND LEAVES A FRESH ONE OPEN — no brick standing, no binding orphaned,
 * and the player still in Build mode.
 *
 * =====================================================================================
 * THE TWO CURRENCIES, AGAIN, AND WHY BOTH ARE NEEDED
 * =====================================================================================
 *
 * `CancelBuild` destroys the structure AND the bricks it spawned. A clear that dropped the binding
 * and left the bricks standing gives the player a plot full of masonry that nothing in the model
 * knows about — colliders with no pieces behind them. A clear that destroyed the bricks and kept the
 * binding gives the opposite: a structure the solver still reasons about with no bricks to show for
 * it. So the world is counted (not one `ABrickActor` that is not the ghost) and the id is read.
 *
 * =====================================================================================
 * "A FRESH BUILD IS OPEN" IS A DIFFERENT ID, NOT MERELY A VALID ONE
 * =====================================================================================
 *
 * `BeginBuild` spends an id on an empty binding, and the id the subsystem hands out is monotonic —
 * so the new build cannot be the old one. Asserting only `!= INDEX_NONE` would pass against a
 * controller that cancelled nothing and kept the same binding, which is precisely the bug where
 * Clear appears to work and the next brick joins a wall that is no longer on screen.
 *
 * AND THE MODE IS UNCHANGED. Clear is a command, not a mode — `SessionToolbarIsActive`'s default arm
 * already says a latched Clear reads as a mode the player is stuck in, and a Clear that dropped the
 * player back into Destroy would be the same confusion one layer up.
 *
 * NEEDS A TICKING WORLD: a world for the spawns and the destroys, but it never ticks one.
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
 * JOINING THE BUILD LEVEL PUTS THE PLAYER IN BUILD MODE WITH A PLOT OPEN — AND JOINING ANY OTHER
 * LEVEL DOES NOT.
 *
 * =====================================================================================
 * WHY BOTH ROWS, IN ONE TEST
 * =====================================================================================
 *
 * "The build level opens in Build mode" is satisfied by a controller that opens EVERY level in Build
 * mode, which is the regression this pairing exists to stop: twenty-eight of the twenty-nine levels
 * lay a structure for the player to pull apart, and a ghost over the middle of one of those walls
 * would swallow the first click on it. So the sandbox row is asserted in the same test, from the
 * same fixture, and the two readings are the same two fields.
 *
 * =====================================================================================
 * AND RUN HAS SOMETHING TO ACT ON ON A SCENARIO LEVEL
 * =====================================================================================
 *
 * `GetSessionStructureId` is what `Run structure` solves, and its fallback is the reason a Destroy
 * session on a scenario level is not inert: the player has laid nothing, so the build component
 * names no structure, and the game mode's own wall is what the button must reach. Asserting it
 * equals `GetBuiltStructureId()` is what makes Run work on the twenty-eight levels that come with a
 * wall already built.
 *
 * ON THE BUILD LEVEL THE GAME MODE BUILDS NOTHING, so the fallback answers INDEX_NONE until the
 * player lays their first brick — which is correct and is exactly why `Run structure` is greyed
 * there until they do. `GameModeOpensAnEmptyBuildSandbox` owns "nothing was laid"; this owns "the
 * session was opened in the right mode".
 *
 * NEEDS A TICKING WORLD: a world with begin-play run under a URL, but it never ticks one. The
 * sandbox arm lays the default wall, which is where its ~200 ms goes.
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
 * THE SESSION'S STRUCTURE IS THE ONE WITH SOMETHING LIVE IN IT: A BUILD WHOSE ONLY BRICK HAS BEEN
 * DELETED MUST NOT SHADOW THE WALL THE LEVEL BUILT.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * After the player lays one brick on a scenario level and then deletes it, `GetSessionStructureId`
 * must name the LEVEL's wall again — because the build now holds a tombstone and nothing else — and
 * the strip must therefore offer `Run structure` live.
 *
 * =====================================================================================
 * TWO READINGS OF "IS THERE A STRUCTURE", AND THEY DISAGREE
 * =====================================================================================
 *
 * `GetSessionStructureId` prefers the player's build "once there is something in it", and asks
 * `NumPieces() > 0`. `RefreshSessionHasStructure` asks the chosen structure for `NumLivePieces() > 0`
 * — and its own header says why: RemovePiece TOMBSTONES rather than compacting, so a plot whose every
 * brick has been deleted still answers a piece count.
 *
 * The two readings are the same answer for every state except exactly one: a build that has had
 * pieces and has none left. There, the first picks the build (one tombstone) and the second then
 * reads zero live pieces off it — so `bHasStructure` goes false and BOTH commands grey themselves,
 * over a 1,220-brick wall standing in front of the player. One deleted brick disables Run for the
 * whole level, and nothing on screen says why. The fix is one reading, used twice.
 *
 * =====================================================================================
 * WHY THE FIXTURE IS A REAL SCENARIO LEVEL
 * =====================================================================================
 *
 * The bug needs BOTH structures to exist at once: a build that is empty-but-not-absent, and a wall
 * behind it for the fallback to reach. A bare world has no wall, so the fallback answers INDEX_NONE
 * and the two readings agree — the test would be green and assert nothing. `?Scenario=sandbox` is
 * the same arm `GameModeOpensBuildModeOnThePlot` uses, and its wall is the one the finding is about.
 *
 * THE BRICK IS LAID THIRTY METRES CLEAR OF THAT WALL, because the Destroy click is a real line trace
 * and a brick inside the wall's span could put one of the level's own bricks under the cursor.
 *
 * =====================================================================================
 * THE STATE IS REFRESHED THROUGH A REAL DOOR BEFORE IT IS READ
 * =====================================================================================
 *
 * The Destroy tab (always live, a bitwise no-op on the state) is clicked before the flag is read,
 * which is what any further interaction with the strip does: `OnToolbarButton` refreshes at the
 * door, precisely so a command is never refused on a stale precondition. The first claim below —
 * `GetSessionStructureId` itself — is asked directly and needs no such door.
 *
 * (THE CLICK IS NOW BELT AND BRACES RATHER THAN THE ONLY ROUTE, and the paragraph that used to
 * stand here said the opposite: at the time `ChoosePieceMenuRow` refreshed nothing, so reading the
 * flag cold would have found the answer from the click that LAID the brick and the claim would have
 * passed without biting. `World.Session.DeleteRefreshesTheSessionFlag` is the test that drove that
 * hole shut — it makes the same read with NO intervening toolbar click, which is the discipline
 * this one deliberately does not have — and the refresh now happens on the delete path itself. The
 * click is kept because it costs nothing and this test is about WHICH STRUCTURE the session names,
 * not about when the flag is refreshed.)
 *
 * NEEDS A TICKING WORLD: a world with begin-play run under a URL — the sandbox row lays its 1,220
 * bricks, which is where the run time goes — and a real line trace against real collision. It never
 * ticks one; nothing here is about anything moving.
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
 * DELETING THE LAST PIECE REFRESHES THE STRIP'S ONE PRECONDITION: `bHasStructure` READS FALSE
 * IMMEDIATELY, WITHOUT WAITING FOR THE NEXT TOOLBAR CLICK.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * After the player lays one brick and then deletes it through the piece menu, the session state
 * read COLD — with no intervening `OnToolbarButton` — says there is nothing to command, so the
 * strip on screen greys `Clear build` and `Run structure` instead of offering them over an empty
 * plot.
 *
 * =====================================================================================
 * WHY THE READ IS COLD, AND WHY THAT IS THE WHOLE TEST
 * =====================================================================================
 *
 * `OnToolbarButton` refreshes the precondition at the door, deliberately — its header says why: a
 * command refused on a stale flag reads as a dropped click. The DELETE path had no such door.
 * `ChoosePieceMenuRow` → `CommitPieceActionForAll` called neither `RefreshSessionHasStructure` nor
 * `RefreshSessionToolbar`, so the flag on the state went on carrying the answer from the click that
 * LAID the brick, and the strip actually on screen went on being drawn from it: the player saw two
 * live commands over a plot with nothing on it, and pressing Run solved an empty graph and reported
 * success. `ChoosePieceMenuRow` now refreshes both on a committed action, and this is the test that
 * holds it there.
 *
 * `World.Session.SessionStructureIsTheLiveOne` reaches its flag through a deliberate Destroy-tab
 * click and its header says so out loud, precisely because this hole was known and not yet driven
 * by a test. This is that test, and its entire discipline is the ABSENCE of that click: any
 * `OnToolbarButton` between the delete and the read makes it pass without biting.
 *
 * =====================================================================================
 * A BARE WORLD, WITH NO LEVEL WALL BEHIND THE BUILD
 * =====================================================================================
 *
 * The sibling above needs a scenario level because its bug is about which of TWO structures wins.
 * This one is the opposite and must have only one: with a wall standing behind the build,
 * `GetSessionStructureId` would fall back to it and `bHasStructure` would be correctly true, so the
 * claim would be unfalsifiable. A bare `FBrickTestWorld` runs no game mode of this class, so the
 * fallback answers `INDEX_NONE` and "there is nothing to command" is simply the truth — which is
 * asserted beside the flag rather than assumed, so a failure says which of the two is wrong.
 *
 * =====================================================================================
 * WHAT IS ASSERTED
 * =====================================================================================
 *
 * The flag, the structure id it should have been derived from, and what the STRIP would draw from
 * it — `SessionToolbarButtons` asked for `RunStructure`. The last is the player-facing statement
 * and the reason the first two matter: a lit command over an empty plot is a button that does
 * nothing, which `Core/SessionToolbar.h` already argues is indistinguishable from a missed click.
 *
 * NEEDS A TICKING WORLD: a world for the spawns and for the Destroy ray's real line trace against
 * real brick collision, but it never ticks one.
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
	 * FROM HERE TO THE END OF THIS TEST, NOTHING MAY TOUCH THE TOOLBAR.
	 *
	 * `OnToolbarButton` refreshes the precondition at its door, so a single click of any button —
	 * even one the strip refuses — would make every claim below pass while the defect stands. The
	 * whole of this test is the gap between the delete and the read.
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

#endif // WITH_DEV_AUTOMATION_TESTS
