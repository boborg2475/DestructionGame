// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/SessionToolbar.h"
#include "DestructionGamePlayerController.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/BuildModeComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named namespace, and named differently from every other one in this module — an anonymous
 * namespace is private to a translation unit, not a file, and a unity build merges many files
 * into one. This is NOT SessionControllerTestSupport, its sibling file, and every name here
 * carries a Shortcut prefix so none of them can be ambiguous against its. `using namespace`
 * lives inside each RunTest for the same reason.
 */
namespace SessionShortcutTestSupport
{
	using namespace DestructionSession;

	const TCHAR* ShortcutModeName(ESessionMode Mode)
	{
		switch (Mode)
		{
		case ESessionMode::Build:   return TEXT("Build");
		case ESessionMode::Destroy: return TEXT("Destroy");
		default:                    return TEXT("<unknown mode>");
		}
	}

	const TCHAR* ShortcutPieceName(EBuildPieceKind Kind)
	{
		switch (Kind)
		{
		case EBuildPieceKind::Brick:        return TEXT("Brick");
		case EBuildPieceKind::TimberPlate:  return TEXT("TimberPlate");
		case EBuildPieceKind::TimberLintel: return TEXT("TimberLintel");
		default:                            return TEXT("<unknown piece>");
		}
	}

	const TCHAR* ShortcutPlacementName(EPlacementMode Placement)
	{
		switch (Placement)
		{
		case EPlacementMode::Snap: return TEXT("Snap");
		case EPlacementMode::Free: return TEXT("Free");
		default:                   return TEXT("<unknown placement>");
		}
	}

	/** The whole session state on one line, so a failure reads without a debugger. */
	FString ShortcutStateBits(const FSessionToolbarState& State)
	{
		return FString::Printf(
			TEXT("{mode %s, piece %s, placement %s, course %d, hasStructure %d}"),
			ShortcutModeName(State.Mode), ShortcutPieceName(State.Piece),
			ShortcutPlacementName(State.Placement), State.Course, State.bHasStructure ? 1 : 0);
	}

	/**
	 * Whether two states are the SAME state, field for field.
	 *
	 * FIELD BY FIELD RATHER THAN memcmp, because a struct with a bool in it carries padding and two
	 * states that differ only in their padding are the same state. The model's refusal contract is a
	 * BITWISE no-op, and this is the honest reading of it.
	 */
	bool ShortcutSameState(const FSessionToolbarState& A, const FSessionToolbarState& B)
	{
		return A.Mode == B.Mode
			&& A.Piece == B.Piece
			&& A.Placement == B.Placement
			&& A.Course == B.Course
			&& A.bHasStructure == B.bHasStructure;
	}

	/**
	 * Course 0's build plane for a brick, spelled out rather than imported (DESIGN §8, 2026-09-15).
	 *
	 * A brick is 6.5 cm tall on a 1 cm bed joint, so the course pitch is 7.5 and the brick's own
	 * half height is 3.25; course 0 therefore centres it at 0 * 7.5 + 3.25 with its underside ON the
	 * earth. Calling DestructionSession::CoursePlaneZCm here would make this file agree with the
	 * plane function however wrong it is, which is the one thing it is watching for.
	 */
	constexpr double ShortcutBrickPlaneCourse0Cm = 3.25;
}

/**
 * S6 — a keyboard shortcut is a toolbar click: the two toggles read the session, dispatch the
 * button the strip would have drawn, and are refused wherever that button is.
 *
 * THE BEHAVIOUR. `ToggleSessionMode()` dispatches `ModeDestroy` when the session is in Build and
 * `ModeBuild` otherwise; `ToggleSessionPlacement()` dispatches `PlacementFree` when Snap and
 * `PlacementSnap` otherwise; both go through `OnToolbarButton`, so toggling a control the current
 * mode does not draw changes nothing and says so.
 *
 * WHY A SEAM PER TOGGLE, AND ONLY FOR THE TWO. Six of the eight shortcuts are a constant: `1` is
 * `PieceBrick`, `]` is `CourseUp`, `Enter` is `RunStructure`. Their handlers are just
 * `OnToolbarButton(Id)`, and `World.Session.ToolbarDrivesTheSession` already pins what that door
 * does with each id, refusals included — a forwarder wired through it needs no second test.
 * `Tab` and `G` differ: SESSION_UI_DESIGN §b makes each ONE binding standing for TWO buttons, so
 * each carries a READ of the current state and a choice between two ids — the only decision in
 * the whole keyboard, and the one thing that can be wrong in a way a player would notice (a
 * `Tab` that always dispatched `ModeBuild` is a key that takes you into Build mode and then
 * appears to jam).
 *
 * THE REFUSAL IS THE HALF THAT CANNOT BE GOT RIGHT BY ACCIDENT. Snap/Free is not on the Destroy
 * strip. A toggle implemented as `State.Placement = Other` — shorter than the right shape — would
 * silently flip a setting in a mode that does not draw it, and the player would come back to
 * Build mode to find the ghost dropping bricks wherever the cursor is. `Core/SessionToolbar.h`
 * states the rule enforced here one layer up: the model consults `SessionToolbarButtons` for
 * whether a click can happen at all, so the CONTROLLER must route every input through
 * `ApplyToolbarButton` and never set fields beside it — a keyboard is an input too. So the
 * refusal is read three ways, each catching a different mistake: the returned bool (a silent
 * no-op reads as a dropped keypress), the state bitwise (a field written beside the model), and
 * the build COMPONENT (a side effect that ran anyway — the failure `OnToolbarButton`'s own header
 * describes, where a greyed `Course down` still lowers the build plane).
 *
 * AND THE MODE TOGGLE IS ASSERTED ON WHAT IT DID, NOT ONLY WHERE IT LANDED. Entering Build mode
 * OPENS a build, `OnToolbarButton(ModeBuild)`'s side effect and nothing a field write would do —
 * the component naming a real structure afterwards is what says the toggle went through the
 * door rather than around it.
 *
 * NEEDS A TICKING WORLD: a world, because the build component spawns a real structure and a real
 * ghost actor, and a real local player so the controller is the one the engine set input up for.
 * It never ticks one; nothing here is about anything moving.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionShortcutsGoThroughTheOneDoorTest,
	"DestructionGame.World.Session.ShortcutsGoThroughTheOneDoor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionShortcutsGoThroughTheOneDoorTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionShortcutTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
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

	/* --- ONE: Tab out of the mode every scenario level opens in --------------------------- */

	TestTrue(
		*FString::Printf(
			TEXT("fixture: a fresh controller opens in Destroy mode; it is %s"),
			*ShortcutStateBits(Controller->GetSessionToolbarState())),
		Controller->GetSessionToolbarState().Mode == ESessionMode::Destroy);

	{
		const bool bToggled = Controller->ToggleSessionMode();

		TestTrue(
			FString::Printf(
				TEXT("the mode tabs are always live, so Tab must report that it landed; it reported %d"),
				bToggled ? 1 : 0),
			bToggled);

		TestTrue(
			*FString::Printf(
				TEXT("TAB OUT OF DESTROY IS BUILD: a toggle that always dispatched one id would be a "
					 "key that appears to jam in whichever mode it favours. The state is %s"),
				*ShortcutStateBits(Controller->GetSessionToolbarState())),
			Controller->GetSessionToolbarState().Mode == ESessionMode::Build);

		/*
		 * And it went through the door: opening a build is ModeBuild's side effect, so a shortcut
		 * that set the mode field beside the model would leave the player in Build mode with no
		 * structure behind it — a mode in which every click fails closed and nothing says why.
		 */
		TestTrue(
			FString::Printf(
				TEXT("and it must have OPENED a build, which is the side effect only the one door "
					 "runs; the component names %d"),
				Build->GetStructureId()),
			Build->GetStructureId() != INDEX_NONE);
	}

	/* --- TWO: G is one binding for two buttons -------------------------------------------- */

	{
		TestTrue(
			*FString::Printf(
				TEXT("fixture: the session starts on Snap placement; it is %s"),
				*ShortcutStateBits(Controller->GetSessionToolbarState())),
			Controller->GetSessionToolbarState().Placement == EPlacementMode::Snap);

		const bool bToFree = Controller->ToggleSessionPlacement();

		TestTrue(
			FString::Printf(
				TEXT("G on a snapped session must report that it landed; it reported %d"),
				bToFree ? 1 : 0),
			bToFree);

		TestTrue(
			*FString::Printf(
				TEXT("and put the session on Free placement; it is %s"),
				*ShortcutStateBits(Controller->GetSessionToolbarState())),
			Controller->GetSessionToolbarState().Placement == EPlacementMode::Free);

		TestTrue(
			FString::Printf(
				TEXT("and the component must have been told — it is the field the subsystem reads to "
					 "honour the cursor verbatim; it reads %s"),
				ShortcutPlacementName(Build->PlacementMode)),
			Build->PlacementMode == EPlacementMode::Free);

		const bool bBackToSnap = Controller->ToggleSessionPlacement();

		TestTrue(
			FString::Printf(
				TEXT("and G again must come back; it reported %d"), bBackToSnap ? 1 : 0),
			bBackToSnap);

		TestTrue(
			*FString::Printf(
				TEXT("to Snap, which is the whole meaning of a segmented control on one key; it is %s"),
				*ShortcutStateBits(Controller->GetSessionToolbarState())),
			Controller->GetSessionToolbarState().Placement == EPlacementMode::Snap);

		TestTrue(
			FString::Printf(
				TEXT("with the component back on Snap too; it reads %s"),
				ShortcutPlacementName(Build->PlacementMode)),
			Build->PlacementMode == EPlacementMode::Snap);
	}

	/* --- THREE: Tab back, and the placement toggle is now REFUSED -------------------------- */

	{
		TestTrue(
			TEXT("Tab back out of Build must land"),
			Controller->ToggleSessionMode());

		TestTrue(
			*FString::Printf(
				TEXT("Tab is a toggle, so it must come back: the state is %s"),
				*ShortcutStateBits(Controller->GetSessionToolbarState())),
			Controller->GetSessionToolbarState().Mode == ESessionMode::Destroy);

		const FSessionToolbarState Before = Controller->GetSessionToolbarState();
		const EPlacementMode ComponentBefore = Build->PlacementMode;

		const bool bToggled = Controller->ToggleSessionPlacement();

		TestTrue(
			FString::Printf(
				TEXT("Snap/Free is not on the Destroy strip, so G there must report that it did "
					 "nothing — a silent no-op on a key reads as a dropped press; it reported %d"),
				bToggled ? 1 : 0),
			!bToggled);

		TestTrue(
			*FString::Printf(
				TEXT("and change nothing: %s against %s. A toggle written as 'set the other value' "
					 "would flip a setting in a mode that does not draw it, and the player would come "
					 "back to Build to find bricks landing wherever the cursor is"),
				*ShortcutStateBits(Controller->GetSessionToolbarState()), *ShortcutStateBits(Before)),
			ShortcutSameState(Controller->GetSessionToolbarState(), Before));

		TestTrue(
			FString::Printf(
				TEXT("and the component untouched: a refused click that still pushed its own side "
					 "effect is the failure OnToolbarButton's header describes. It reads %s against %s"),
				ShortcutPlacementName(Build->PlacementMode), ShortcutPlacementName(ComponentBefore)),
			Build->PlacementMode == ComponentBefore);
	}

	/* --- FOUR: and the course key cannot dig below the earth either ------------------------ */

	{
		TestTrue(TEXT("Tab back into Build"), Controller->ToggleSessionMode());

		TestEqual(
			FString::Printf(
				TEXT("fixture: nothing has moved the course, so the session is on 0; it reads %d"),
				Controller->GetSessionToolbarState().Course),
			Controller->GetSessionToolbarState().Course, 0);

		const FSessionToolbarState Before = Controller->GetSessionToolbarState();

		/*
		 * The `[` handler is this call — no seam of its own, since the shortcut and the chip
		 * dispatch the same constant id. What is asserted is that the keyboard gets the same
		 * refusal the greyed chip does: a course key wired straight at the component, or at the
		 * state field, is the shape that puts the build plane under the ground with the readout
		 * still saying course 0.
		 */
		const bool bSteppedBelow = Controller->OnToolbarButton(EToolbarButtonId::CourseDown);

		TestTrue(
			FString::Printf(
				TEXT("there is no course below the one with the earth under it, whichever input asks: "
					 "the key must report that it did nothing; it reported %d"),
				bSteppedBelow ? 1 : 0),
			!bSteppedBelow);

		TestTrue(
			*FString::Printf(
				TEXT("and leave the state exactly as it was — %s against %s"),
				*ShortcutStateBits(Controller->GetSessionToolbarState()), *ShortcutStateBits(Before)),
			ShortcutSameState(Controller->GetSessionToolbarState(), Before));

		TestEqual(
			FString::Printf(
				TEXT("with the build plane still on a brick's course 0, 0 * 7.5 + 3.25 = %g; it is %g"),
				ShortcutBrickPlaneCourse0Cm, Build->BuildPlaneZCm),
			Build->BuildPlaneZCm, ShortcutBrickPlaneCourse0Cm);
	}

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
