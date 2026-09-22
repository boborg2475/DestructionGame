// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/SessionToolbar.h"
#include "DestructionGamePlayerController.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/BuildModeComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named namespace with Shortcut-prefixed names, so unity builds do not collide with
 * SessionControllerTestSupport.
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

	/** Field-by-field equality; memcmp would compare padding. */
	bool ShortcutSameState(const FSessionToolbarState& A, const FSessionToolbarState& B)
	{
		return A.Mode == B.Mode
			&& A.Piece == B.Piece
			&& A.Placement == B.Placement
			&& A.Course == B.Course
			&& A.bHasStructure == B.bHasStructure;
	}

	/** Course 0's brick plane (half of 6.5 cm), written out rather than imported (DESIGN §8). */
	constexpr double ShortcutBrickPlaneCourse0Cm = 3.25;
}

/**
 * S6: a keyboard shortcut is a toolbar click. ToggleSessionMode and ToggleSessionPlacement read the
 * state, pick one of two button ids, and go through OnToolbarButton, so they are refused wherever
 * the strip does not draw that button.
 *
 * Only these two toggles need a test: the other shortcuts forward a constant id, which
 * World.Session.ToolbarDrivesTheSession already covers. These two choose between ids
 * (SESSION_UI_DESIGN §b), so a toggle that always picked one would appear to jam.
 *
 * The refusal (G in Destroy mode) is checked three ways: the returned bool, the state field by
 * field, and the build component, catching a silent no-op, a field written beside the model, and
 * a side effect that ran anyway. The mode toggle is also checked by its side effect: entering
 * Build opens a structure.
 *
 * Needs a world and a local player for the build component and controller; never ticks.
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

	/* --- 1: Tab out of Destroy -------------------------------------------------------------- */

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

		// Opening a build is ModeBuild's side effect, so a field write alone would leave none.
		TestTrue(
			FString::Printf(
				TEXT("and it must have OPENED a build, which is the side effect only the one door "
					 "runs; the component names %d"),
				Build->GetStructureId()),
			Build->GetStructureId() != INDEX_NONE);
	}

	/* --- 2: G is one binding for two buttons ------------------------------------------------ */

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

	/* --- 3: Tab back; the placement toggle is now refused ----------------------------------- */

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

	/* --- 4: the course key cannot go below course 0 ----------------------------------------- */

	{
		TestTrue(TEXT("Tab back into Build"), Controller->ToggleSessionMode());

		TestEqual(
			FString::Printf(
				TEXT("fixture: nothing has moved the course, so the session is on 0; it reads %d"),
				Controller->GetSessionToolbarState().Course),
			Controller->GetSessionToolbarState().Course, 0);

		const FSessionToolbarState Before = Controller->GetSessionToolbarState();

		// The `[` handler is exactly this call; it must get the greyed chip's refusal.
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
