// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/BuildMode/DemoBuilding.h"
#include "Core/BuildMode/SnapSolver.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/SessionToolbar.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges many
 * files into one. See CURRENT_STATE.md and Tests/ConnectionLoadTest.cpp, which is where that rule
 * was paid for; the `using namespace` lives inside each RunTest for the same reason.
 */
namespace SessionToolbarTestSupport
{
	/**
	 * THE BRICK COURSE PITCH, TRANSCRIBED RATHER THAN IMPORTED, AND IT IS 7.5 cm.
	 *
	 * The production function is required to DERIVE it — FSnapSettings::BrickSizeCm.Z (6.5) plus
	 * JointThicknessCm (1.0) — because a course is the brick's own coordinating dimension and a
	 * second hand-written 7.5 in Core would be a coordinating grid spelled in two places. What this
	 * test does is the opposite of importing it: it spells the sum out itself, against a default
	 * FSnapSettings, so that a retune of the brick or the joint fails HERE rather than quietly
	 * agreeing with whatever the plane function now says.
	 */
	constexpr double BrickCoursePitchCm = 7.5;

	/** The brick's half height — half of FSnapSettings::BrickSizeCm.Z, the standard 6.5 cm unit. */
	constexpr double BrickHalfHeightCm = 3.25;

	/** The wall plate's half height, from the demo building's own plate extent (33.75, 5.125, 5.0). */
	constexpr double PlateHalfHeightCm = 5.0;

	/**
	 * WHICH PIECE OF THE DEMO BUILDING EACH COURSE'S HEIGHT IS READ OFF, by placement index.
	 *
	 * THE HEIGHTS THEMSELVES ARE NO LONGER TRANSCRIBED. They used to be three literals copied out
	 * of Core/BuildMode/DemoBuilding.cpp (0, 7.5, 16.75) — which pinned the demo's numbers in a
	 * second file, so migrating the demo onto the rests-on-the-ground convention would have left
	 * this test agreeing with a building that no longer exists. Running the builder and reading the
	 * placed centres out of its own FBrickLayout makes the comparison live: the day the demo moves,
	 * the lift here stops being one brick half-height and this test says so.
	 *
	 * The indices are the builder's documented placement order (Tests/DemoBuildingTest.cpp pins the
	 * whole eight-step sequence): 0-3 are the grounded course, 4-6 the staggered course above it,
	 * and 7 the timber wall plate bearing across the top.
	 */
	constexpr int32 DemoCourseZeroPieceIndex = 0;
	constexpr int32 DemoCourseOnePieceIndex = 4;
	constexpr int32 DemoPlatePieceIndex = 7;

	const TCHAR* NameOfMode(DestructionSession::ESessionMode Mode)
	{
		switch (Mode)
		{
		case DestructionSession::ESessionMode::Build:   return TEXT("Build");
		case DestructionSession::ESessionMode::Destroy: return TEXT("Destroy");
		}

		return TEXT("<not a session mode>");
	}

	const TCHAR* NameOfPiece(DestructionSession::EBuildPieceKind Kind)
	{
		switch (Kind)
		{
		case DestructionSession::EBuildPieceKind::Brick:        return TEXT("Brick");
		case DestructionSession::EBuildPieceKind::TimberPlate:  return TEXT("TimberPlate");
		case DestructionSession::EBuildPieceKind::TimberLintel: return TEXT("TimberLintel");
		}

		return TEXT("<not a piece kind>");
	}

	const TCHAR* NameOfPlacement(DestructionSession::EPlacementMode Placement)
	{
		switch (Placement)
		{
		case DestructionSession::EPlacementMode::Snap: return TEXT("Snap");
		case DestructionSession::EPlacementMode::Free: return TEXT("Free");
		}

		return TEXT("<not a placement mode>");
	}

	const TCHAR* NameOfButton(DestructionSession::EToolbarButtonId Id)
	{
		using namespace DestructionSession;

		switch (Id)
		{
		case EToolbarButtonId::ModeBuild:         return TEXT("ModeBuild");
		case EToolbarButtonId::ModeDestroy:       return TEXT("ModeDestroy");
		case EToolbarButtonId::PieceBrick:        return TEXT("PieceBrick");
		case EToolbarButtonId::PieceTimberPlate:  return TEXT("PieceTimberPlate");
		case EToolbarButtonId::PieceTimberLintel: return TEXT("PieceTimberLintel");
		case EToolbarButtonId::PlacementSnap:     return TEXT("PlacementSnap");
		case EToolbarButtonId::PlacementFree:     return TEXT("PlacementFree");
		case EToolbarButtonId::CourseDown:        return TEXT("CourseDown");
		case EToolbarButtonId::CourseUp:          return TEXT("CourseUp");
		case EToolbarButtonId::ClearBuild:        return TEXT("ClearBuild");
		case EToolbarButtonId::RunStructure:      return TEXT("RunStructure");
		}

		return TEXT("<not a toolbar button>");
	}

	/** Every button the model knows, so a sweep covers the whole vocabulary rather than a favourite few. */
	TArray<DestructionSession::EToolbarButtonId> AllButtonIds()
	{
		using namespace DestructionSession;

		return {
			EToolbarButtonId::ModeBuild,
			EToolbarButtonId::ModeDestroy,
			EToolbarButtonId::PieceBrick,
			EToolbarButtonId::PieceTimberPlate,
			EToolbarButtonId::PieceTimberLintel,
			EToolbarButtonId::PlacementSnap,
			EToolbarButtonId::PlacementFree,
			EToolbarButtonId::CourseDown,
			EToolbarButtonId::CourseUp,
			EToolbarButtonId::ClearBuild,
			EToolbarButtonId::RunStructure,
		};
	}

	FString DescribeState(const DestructionSession::FSessionToolbarState& State)
	{
		return FString::Printf(
			TEXT("{%s, %s, %s, Course %d, %s}"),
			NameOfMode(State.Mode),
			NameOfPiece(State.Piece),
			NameOfPlacement(State.Placement),
			State.Course,
			State.bHasStructure ? TEXT("has a structure") : TEXT("no structure"));
	}

	FString DescribeButtons(const TArray<DestructionSession::FToolbarButton>& Buttons)
	{
		if (Buttons.Num() == 0)
		{
			return TEXT("<empty>");
		}

		FString Line;

		for (int32 Index = 0; Index < Buttons.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s%s('%s'%s%s)"),
				Index == 0 ? TEXT("") : TEXT(", "),
				NameOfButton(Buttons[Index].Id),
				*Buttons[Index].Label,
				Buttons[Index].bActive ? TEXT(" active") : TEXT(""),
				Buttons[Index].bEnabled ? TEXT("") : TEXT(" DISABLED"));
		}

		return Line;
	}

	/**
	 * FIELD BY FIELD RATHER THAN FMemory::Memcmp, WHICH IS THE HONEST SPELLING OF "UNCHANGED".
	 *
	 * The struct has padding between its bool and whatever follows, and padding is not required to
	 * be copied — so a memcmp would report a difference nobody can see and would do it
	 * intermittently. Every field the state HAS is compared here, so adding one without extending
	 * this helper is the only way a change can hide, and that is a mechanical omission rather than
	 * a silent one.
	 */
	bool StatesEqual(
		const DestructionSession::FSessionToolbarState& A,
		const DestructionSession::FSessionToolbarState& B)
	{
		return A.Mode == B.Mode
			&& A.Piece == B.Piece
			&& A.Placement == B.Placement
			&& A.Course == B.Course
			&& A.bHasStructure == B.bHasStructure;
	}

	const DestructionSession::FToolbarButton* FindButton(
		const TArray<DestructionSession::FToolbarButton>& Buttons,
		DestructionSession::EToolbarButtonId Id)
	{
		for (const DestructionSession::FToolbarButton& Button : Buttons)
		{
			if (Button.Id == Id)
			{
				return &Button;
			}
		}

		return nullptr;
	}

	DestructionSession::FSessionToolbarState MakeState(
		DestructionSession::ESessionMode Mode,
		DestructionSession::EBuildPieceKind Piece,
		DestructionSession::EPlacementMode Placement,
		int32 Course,
		bool bHasStructure)
	{
		DestructionSession::FSessionToolbarState State;
		State.Mode = Mode;
		State.Piece = Piece;
		State.Placement = Placement;
		State.Course = Course;
		State.bHasStructure = bHasStructure;
		return State;
	}

	/**
	 * THE WHOLE STATE SPACE THE FLAG RULES ARE CLAIMED OVER, rather than the three or four states
	 * somebody thought of.
	 *
	 * Every rule about which buttons exist, which is lit and which is greyed is a claim about ALL
	 * states, and a hand-picked handful covers only the shapes the author had in mind — DESIGN §4's
	 * "an invariant asserted over fixtures that all share a hidden property is not an invariant".
	 * Two modes x three pieces x two placements x three courses x two structure flags is 72 states
	 * and costs microseconds, so the sweep is the cheap way to be sure the hidden property is not
	 * "the author always wrote Course 0".
	 *
	 * The courses are 0 (the grounded course, where CourseDown must be refused), 1 (the first course
	 * where it must be offered) and 5 (well clear of the boundary, so an off-by-one at 1 cannot be
	 * the only thing the boundary rows see).
	 */
	TArray<DestructionSession::FSessionToolbarState> AllStates()
	{
		using namespace DestructionSession;

		const ESessionMode Modes[] = { ESessionMode::Build, ESessionMode::Destroy };
		const EBuildPieceKind Pieces[] = {
			EBuildPieceKind::Brick, EBuildPieceKind::TimberPlate, EBuildPieceKind::TimberLintel };
		const EPlacementMode Placements[] = { EPlacementMode::Snap, EPlacementMode::Free };
		const int32 Courses[] = { 0, 1, 5 };
		const bool Structures[] = { false, true };

		TArray<FSessionToolbarState> States;

		for (ESessionMode Mode : Modes)
		{
			for (EBuildPieceKind Piece : Pieces)
			{
				for (EPlacementMode Placement : Placements)
				{
					for (int32 Course : Courses)
					{
						for (bool bHasStructure : Structures)
						{
							States.Add(MakeState(Mode, Piece, Placement, Course, bHasStructure));
						}
					}
				}
			}
		}

		return States;
	}

	bool VectorsExactlyEqual(const FVector& A, const FVector& B)
	{
		return A.X == B.X && A.Y == B.Y && A.Z == B.Z;
	}

	FString DescribeVector(const FVector& V)
	{
		return FString::Printf(TEXT("(%g, %g, %g)"), V.X, V.Y, V.Z);
	}

	/** Which shipped profile a returned reference IS, by address. Never a value comparison. */
	const TCHAR* NameOfMaterialByAddress(const DestructionProfiles::FMaterialProfile& Material)
	{
		if (&Material == &DestructionProfiles::ClayBrick)         { return TEXT("ClayBrick"); }
		if (&Material == &DestructionProfiles::Timber)            { return TEXT("Timber"); }
		if (&Material == &DestructionProfiles::StructuralConcrete) { return TEXT("StructuralConcrete"); }

		return TEXT("<not a shipped library row>");
	}

	bool IsAShippedProfile(const DestructionProfiles::FMaterialProfile& Material)
	{
		return &Material == &DestructionProfiles::ClayBrick
			|| &Material == &DestructionProfiles::Timber
			|| &Material == &DestructionProfiles::StructuralConcrete;
	}
}

/**
 * THE TOOLBAR DRAWS ONE FIXED LIST PER MODE, IN ONE FIXED ORDER, AND THE MODE PAIR IS ALWAYS FIRST.
 *
 * WHY THE LIST IS A MODEL ANSWER AND NOT A SLATE LAYOUT. What a player sees is a strip of buttons,
 * and asserting that widgets appeared needs a viewport, fails for reasons that have nothing to do
 * with the toolbar, and cannot see the failures that matter: a button offered in the mode it does
 * nothing in, a button missing, or the strip reordering under the player's cursor between two
 * clicks. All three are a pure function of the state, so they live here — exactly as
 * BuildPieceMenuRows and PieceMenuPanelSizePx do for the piece menu, and for the same reason.
 *
 * THE MODE PAIR BEING FIRST IS THE ONE ORDERING CLAIM WITH A PLAYER-FACING REASON. Everything else
 * on the strip changes with the mode; the two buttons that switch modes may not move, because a
 * strip whose first two slots shifted would put a different button under a cursor that has not
 * moved. That is a decision, and a decision expressed as the order of AddSlot calls in a widget is
 * a decision in the one place no test can reach.
 *
 * SWEPT OVER THE WHOLE STATE SPACE rather than one state per mode: the list's CONTENT depends on
 * the mode alone, and that is itself the claim — a piece selection or a course number that added,
 * removed or reordered a button would be a strip that rearranges itself while a player uses it.
 *
 * NEEDS A TICKING WORLD: no, and not even a world. One plain struct in, an array of plain structs
 * out.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarButtonsByModeTest,
	"DestructionGame.Core.SessionToolbar.ButtonsByMode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarButtonsByModeTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	const TArray<EToolbarButtonId> ExpectedInBuild = {
		EToolbarButtonId::ModeBuild,
		EToolbarButtonId::ModeDestroy,
		EToolbarButtonId::PieceBrick,
		EToolbarButtonId::PieceTimberPlate,
		EToolbarButtonId::PieceTimberLintel,
		EToolbarButtonId::PlacementSnap,
		EToolbarButtonId::PlacementFree,
		EToolbarButtonId::CourseDown,
		EToolbarButtonId::CourseUp,
		EToolbarButtonId::ClearBuild,
	};

	const TArray<EToolbarButtonId> ExpectedInDestroy = {
		EToolbarButtonId::ModeBuild,
		EToolbarButtonId::ModeDestroy,
		EToolbarButtonId::RunStructure,
	};

	for (const FSessionToolbarState& State : AllStates())
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);
		const TArray<EToolbarButtonId>& Expected =
			State.Mode == ESessionMode::Build ? ExpectedInBuild : ExpectedInDestroy;

		TestEqual(
			FString::Printf(
				TEXT("%s: the toolbar must draw %d buttons, it drew %d — [%s]"),
				*DescribeState(State), Expected.Num(), Buttons.Num(), *DescribeButtons(Buttons)),
			Buttons.Num(), Expected.Num());

		const int32 Common = FMath::Min(Buttons.Num(), Expected.Num());

		for (int32 Index = 0; Index < Common; ++Index)
		{
			TestEqual(
				FString::Printf(
					TEXT("%s: slot %d must be %s, it is %s — [%s]"),
					*DescribeState(State), Index, NameOfButton(Expected[Index]),
					NameOfButton(Buttons[Index].Id), *DescribeButtons(Buttons)),
				static_cast<int32>(Buttons[Index].Id), static_cast<int32>(Expected[Index]));
		}

		/*
		 * AND NO BUTTON APPEARS TWICE. Two slots carrying one id is a strip where a click cannot be
		 * attributed, and it is the failure an ordered element-by-element comparison happens not to
		 * catch when the duplicate lands where its twin was expected.
		 */
		TSet<EToolbarButtonId> Seen;

		for (const FToolbarButton& Button : Buttons)
		{
			bool bAlreadyThere = false;
			Seen.Add(Button.Id, &bAlreadyThere);

			TestFalse(
				*FString::Printf(
					TEXT("%s: %s appears more than once — [%s]"),
					*DescribeState(State), NameOfButton(Button.Id), *DescribeButtons(Buttons)),
				bAlreadyThere);
		}
	}

	return true;
}

/**
 * EXACTLY ONE BUTTON IN EACH SETTING GROUP IS LIT, AND IT IS THE ONE THE STATE NAMES; A COMMAND
 * BUTTON IS NEVER LIT.
 *
 * bActive IS "THIS IS WHAT YOU HAVE CHOSEN", WHICH IS NOT THE SAME QUESTION AS bEnabled. A toolbar
 * with two lit piece buttons tells the player the ghost is two pieces at once; a toolbar with none
 * lit tells them the click they just made did nothing. Both are states a widget that derived its
 * own highlight by comparing captions would reach, which is why the answer is data on the row.
 *
 * COMMAND BUTTONS ARE NEVER ACTIVE, and that is a claim rather than an omission. Clear and Run are
 * things that HAPPEN; a latched-looking Clear button reads as a mode the player is stuck in, and
 * the toolbar has two genuine modes already.
 *
 * NEEDS A TICKING WORLD: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarActiveFlagsTest,
	"DestructionGame.Core.SessionToolbar.ActiveFlags",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarActiveFlagsTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	for (const FSessionToolbarState& State : AllStates())
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);
		const bool bBuilding = State.Mode == ESessionMode::Build;

		int32 ActiveModeButtons = 0;
		int32 ActivePieceButtons = 0;
		int32 ActivePlacementButtons = 0;

		for (const FToolbarButton& Button : Buttons)
		{
			switch (Button.Id)
			{
			case EToolbarButtonId::ModeBuild:
				ActiveModeButtons += Button.bActive ? 1 : 0;
				TestEqual(
					*FString::Printf(
						TEXT("%s: ModeBuild is lit exactly when the session is in Build mode — [%s]"),
						*DescribeState(State), *DescribeButtons(Buttons)),
					Button.bActive, bBuilding);
				break;

			case EToolbarButtonId::ModeDestroy:
				ActiveModeButtons += Button.bActive ? 1 : 0;
				TestEqual(
					*FString::Printf(
						TEXT("%s: ModeDestroy is lit exactly when the session is in Destroy mode — [%s]"),
						*DescribeState(State), *DescribeButtons(Buttons)),
					Button.bActive, !bBuilding);
				break;

			case EToolbarButtonId::PieceBrick:
				ActivePieceButtons += Button.bActive ? 1 : 0;
				TestEqual(
					*FString::Printf(
						TEXT("%s: PieceBrick is lit exactly when Brick is the chosen piece — [%s]"),
						*DescribeState(State), *DescribeButtons(Buttons)),
					Button.bActive, State.Piece == EBuildPieceKind::Brick);
				break;

			case EToolbarButtonId::PieceTimberPlate:
				ActivePieceButtons += Button.bActive ? 1 : 0;
				TestEqual(
					*FString::Printf(
						TEXT("%s: PieceTimberPlate is lit exactly when TimberPlate is the chosen piece — [%s]"),
						*DescribeState(State), *DescribeButtons(Buttons)),
					Button.bActive, State.Piece == EBuildPieceKind::TimberPlate);
				break;

			case EToolbarButtonId::PieceTimberLintel:
				ActivePieceButtons += Button.bActive ? 1 : 0;
				TestEqual(
					*FString::Printf(
						TEXT("%s: PieceTimberLintel is lit exactly when TimberLintel is the chosen piece — [%s]"),
						*DescribeState(State), *DescribeButtons(Buttons)),
					Button.bActive, State.Piece == EBuildPieceKind::TimberLintel);
				break;

			case EToolbarButtonId::PlacementSnap:
				ActivePlacementButtons += Button.bActive ? 1 : 0;
				TestEqual(
					*FString::Printf(
						TEXT("%s: PlacementSnap is lit exactly when placement is Snap — [%s]"),
						*DescribeState(State), *DescribeButtons(Buttons)),
					Button.bActive, State.Placement == EPlacementMode::Snap);
				break;

			case EToolbarButtonId::PlacementFree:
				ActivePlacementButtons += Button.bActive ? 1 : 0;
				TestEqual(
					*FString::Printf(
						TEXT("%s: PlacementFree is lit exactly when placement is Free — [%s]"),
						*DescribeState(State), *DescribeButtons(Buttons)),
					Button.bActive, State.Placement == EPlacementMode::Free);
				break;

			default:
				TestFalse(
					*FString::Printf(
						TEXT("%s: %s is a COMMAND and must never be lit — [%s]"),
						*DescribeState(State), NameOfButton(Button.Id), *DescribeButtons(Buttons)),
					Button.bActive);
				break;
			}
		}

		TestEqual(
			FString::Printf(
				TEXT("%s: exactly one of ModeBuild/ModeDestroy must be lit — [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			ActiveModeButtons, 1);

		/*
		 * THE PIECE AND PLACEMENT GROUPS ARE NOT DRAWN IN DESTROY MODE, so "exactly one lit" is a
		 * claim about the buttons that EXIST. Expecting one in a mode that draws none would assert
		 * the group into existence and contradict the list this file already pins.
		 */
		TestEqual(
			FString::Printf(
				TEXT("%s: exactly one Piece button must be lit when the group is drawn — [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			ActivePieceButtons, bBuilding ? 1 : 0);

		TestEqual(
			FString::Printf(
				TEXT("%s: exactly one Placement button must be lit when the group is drawn — [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			ActivePlacementButtons, bBuilding ? 1 : 0);
	}

	return true;
}

/**
 * A BUTTON IS GREYED EXACTLY WHEN THE THING BEHIND IT CANNOT HAPPEN: CourseDown ON THE GROUND
 * COURSE, AND Clear AND Run WITH NOTHING BUILT.
 *
 * THE MODEL OWNS THE GREYING BECAUSE THE MODEL OWNS THE REFUSAL. ApplyToolbarButton already refuses
 * to take the course below zero; if the widget decided separately whether to grey the button, the
 * refusal would be written twice and the two copies would disagree the day the floor moves — the
 * player would be left clicking a lit button that does nothing, which is the failure this project
 * keeps closing, one layer out.
 *
 * bHasStructure IS THE ONLY PRECONDITION EITHER COMMAND HAS. Run with nothing built solves an empty
 * structure and Clear with nothing built clears nothing; both are silent no-ops, and a silent no-op
 * on a command button is indistinguishable from the game having missed the click.
 *
 * EVERYTHING ELSE IS ALWAYS ENABLED, and that is asserted rather than assumed. A mode button greyed
 * by an over-eager precondition is a player who cannot get out of the mode they are in.
 *
 * NEEDS A TICKING WORLD: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarEnabledFlagsTest,
	"DestructionGame.Core.SessionToolbar.EnabledFlags",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarEnabledFlagsTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	for (const FSessionToolbarState& State : AllStates())
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);

		/*
		 * THE FLOOR, AND IT IS NOT DECORATION — IT IS THE ONLY THING BETWEEN THIS TEST AND PASSING
		 * ON AN EMPTY STRIP. Everything below is a sweep OVER the buttons that came back, so a model
		 * that returned nothing at all would satisfy every claim in the loop by having nothing to
		 * check. Measured: against a stub returning {} this test reported Success until this block
		 * was added. The three buttons named are exactly the ones this test exists for — the only
		 * three with a precondition — so requiring them present is requiring the subject to exist.
		 */
		const bool bBuilding = State.Mode == ESessionMode::Build;

		TestTrue(
			*FString::Printf(
				TEXT("%s: the toolbar must draw buttons at all — [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			Buttons.Num() > 0);

		TestTrue(
			*FString::Printf(
				TEXT("%s: %s must be on the strip for its enablement to mean anything — [%s]"),
				*DescribeState(State),
				bBuilding ? TEXT("CourseDown and ClearBuild") : TEXT("RunStructure"),
				*DescribeButtons(Buttons)),
			bBuilding
				? (FindButton(Buttons, EToolbarButtonId::CourseDown) != nullptr
					&& FindButton(Buttons, EToolbarButtonId::ClearBuild) != nullptr)
				: FindButton(Buttons, EToolbarButtonId::RunStructure) != nullptr);

		for (const FToolbarButton& Button : Buttons)
		{
			bool bExpectedEnabled = true;
			const TCHAR* Why = TEXT("it has no precondition at all");

			switch (Button.Id)
			{
			case EToolbarButtonId::CourseDown:
				bExpectedEnabled = State.Course >= 1;
				Why = TEXT("there is no course below the grounded one");
				break;

			case EToolbarButtonId::ClearBuild:
			case EToolbarButtonId::RunStructure:
				bExpectedEnabled = State.bHasStructure;
				Why = TEXT("it acts on a live structure and there must be one");
				break;

			default:
				break;
			}

			TestEqual(
				*FString::Printf(
					TEXT("%s: %s must be %s (%s), it is %s — [%s]"),
					*DescribeState(State), NameOfButton(Button.Id),
					bExpectedEnabled ? TEXT("ENABLED") : TEXT("DISABLED"), Why,
					Button.bEnabled ? TEXT("enabled") : TEXT("disabled"), *DescribeButtons(Buttons)),
				Button.bEnabled, bExpectedEnabled);
		}
	}

	return true;
}

/**
 * EVERY BUTTON CARRIES A CAPTION, NO TWO CAPTIONS ON ONE STRIP READ THE SAME, AND THE FIVE CAPTIONS
 * A PLAYER NAVIGATES BY SAY THE WORD THEY NAVIGATE BY.
 *
 * THE CAPTION IS DATA ON THE ROW FOR THE REASON FPieceMenuRow::Label IS: a widget that spelled its
 * own button text would be holding wording in the one place no test can reach, and this strip's
 * wording is load-bearing — Snap versus Free is the difference between a piece that lands on the
 * bond and one that lands where the cursor was.
 *
 * WORDING IS BOUNDED, NOT PINNED, AND DELIBERATELY. Pinning "Snap" exactly would make every future
 * retune — "Snap to bond", a keyboard hint in the caption — a test failure with nothing wrong
 * behind it. What may not drift is that the button a player is hunting for contains the word they
 * are hunting for, and that no two buttons on one strip are indistinguishable.
 *
 * DISTINCTNESS IS WITHIN ONE STRIP RATHER THAN GLOBAL, which is what the rule is actually for: two
 * identical captions side by side is a strip a player cannot read. ModeBuild and ClearBuild may
 * both contain the word "Build" and that is fine — they are different sentences.
 *
 * NEEDS A TICKING WORLD: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarLabelsTest,
	"DestructionGame.Core.SessionToolbar.Labels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarLabelsTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	struct FWordCase
	{
		EToolbarButtonId Id;
		const TCHAR* Word;
	};

	const FWordCase RequiredWords[] = {
		{ EToolbarButtonId::ModeBuild,     TEXT("Build") },
		{ EToolbarButtonId::ModeDestroy,   TEXT("Destroy") },
		{ EToolbarButtonId::PlacementSnap, TEXT("Snap") },
		{ EToolbarButtonId::PlacementFree, TEXT("Free") },
		{ EToolbarButtonId::RunStructure,  TEXT("Run") },
	};

	/*
	 * HOW MANY WORD CHECKS THIS SWEEP OWES, COUNTED AS IT GOES AND SETTLED AT THE END.
	 *
	 * THE SAME FLOOR THE ENABLED-FLAGS TEST CARRIES, FOR THE SAME MEASURED REASON: every claim below
	 * is a sweep over the buttons that came back and over the word rows that are actually on the
	 * strip, so a model returning nothing satisfies all of them by having nothing to check —
	 * measured, against a stub returning {}, as a Success. The owed count is accumulated from the
	 * mode rather than written as a literal so that it tracks AllStates() instead of going stale
	 * the day a state is added to the matrix.
	 */
	int32 WordChecksOwed = 0;
	int32 WordChecksMade = 0;

	for (const FSessionToolbarState& State : AllStates())
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);

		/* ModeBuild, ModeDestroy and either the two placement captions or Run. */
		WordChecksOwed += State.Mode == ESessionMode::Build ? 4 : 3;

		TestTrue(
			*FString::Printf(
				TEXT("%s: the toolbar must draw buttons at all — [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			Buttons.Num() > 0);

		TSet<FString> SeenLabels;

		for (const FToolbarButton& Button : Buttons)
		{
			TestFalse(
				*FString::Printf(
					TEXT("%s: %s must carry a caption, it is empty — [%s]"),
					*DescribeState(State), NameOfButton(Button.Id), *DescribeButtons(Buttons)),
				Button.Label.IsEmpty());

			bool bAlreadyThere = false;
			SeenLabels.Add(Button.Label, &bAlreadyThere);

			TestFalse(
				*FString::Printf(
					TEXT("%s: two buttons on one strip both read '%s' — [%s]"),
					*DescribeState(State), *Button.Label, *DescribeButtons(Buttons)),
				bAlreadyThere);
		}

		for (const FWordCase& Case : RequiredWords)
		{
			const FToolbarButton* Button = FindButton(Buttons, Case.Id);

			if (Button == nullptr)
			{
				// Not drawn in this mode; the list test owns which modes draw what.
				continue;
			}

			++WordChecksMade;

			TestTrue(
				*FString::Printf(
					TEXT("%s: %s's caption must contain '%s', it reads '%s'"),
					*DescribeState(State), NameOfButton(Case.Id), Case.Word, *Button->Label),
				Button->Label.Contains(Case.Word));
		}
	}

	TestEqual(
		FString::Printf(
			TEXT("the sweep must actually have read %d captions, it read %d — a strip that drew none would pass every claim above by having nothing to check"),
			WordChecksOwed, WordChecksMade),
		WordChecksMade, WordChecksOwed);

	return true;
}

/**
 * ONE CLICK CHANGES ONE THING AND LEAVES EVERYTHING ELSE ALONE; A CLICK ON A BUTTON THAT IS GREYED
 * OR NOT ON SCREEN CHANGES NOTHING AT ALL.
 *
 * THE TRANSITION IS A PURE FUNCTION BECAUSE THAT IS THE ONLY WAY IT IS ASSERTABLE. A controller that
 * mutated its own fields from a Slate callback would put the whole of the toolbar's behaviour behind
 * a click that only a human can perform; state-in, state-out makes every transition a table row.
 *
 * THE FIELDS A TRANSITION DOES NOT NAME MUST SURVIVE IT, and the rows below are built to catch the
 * opposite: they start from states with a non-default piece, a non-default placement and a non-zero
 * course, so a transition that rebuilt the state from scratch — the natural way to write one — fails
 * rather than agreeing by coincidence with a default-constructed expectation.
 *
 * THE COURSE FLOOR IS THE FAIL-CLOSED ROW. Course is a player-facing readout and a build plane; a
 * negative one is a plane under the ground, which is why the refusal is here rather than left to the
 * caller and why the greyed CourseDown must be a BITWISE no-op rather than a clamp that happens to
 * land on the same number. Those are the same answer today and stop being the same answer the moment
 * anything else on the state moves with a course change.
 *
 * AND THE LAST BLOCK HOLDS THE TWO FUNCTIONS AGAINST EACH OTHER over the whole state space: whatever
 * SessionToolbarButtons refuses to draw, or draws greyed, ApplyToolbarButton must refuse to act on.
 * Two separately-written answers to "can this happen" is exactly how a lit button that does nothing
 * gets shipped.
 *
 * NEEDS A TICKING WORLD: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarTransitionsTest,
	"DestructionGame.Core.SessionToolbar.Transitions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarTransitionsTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	struct FTransitionCase
	{
		const TCHAR* Description;
		FSessionToolbarState Before;
		EToolbarButtonId Click;
		FSessionToolbarState Expected;
	};

	const FTransitionCase Cases[] = {
		{
			TEXT("Destroy takes the session into destroy mode and keeps every build setting for the way back"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 3, true),
			EToolbarButtonId::ModeDestroy,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 3, true),
		},
		{
			TEXT("Build takes it back, and the settings are still the ones it left with"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 3, true),
			EToolbarButtonId::ModeBuild,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 3, true),
		},
		{
			TEXT("clicking the mode you are already in changes nothing"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 2, true),
			EToolbarButtonId::ModeBuild,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 2, true),
		},
		{
			TEXT("choosing the plate changes the piece and nothing else"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Free, 4, true),
			EToolbarButtonId::PieceTimberPlate,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 4, true),
		},
		{
			TEXT("choosing the lintel changes the piece and nothing else"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 4, false),
			EToolbarButtonId::PieceTimberLintel,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Snap, 4, false),
		},
		{
			TEXT("and choosing the brick back again"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 1, true),
			EToolbarButtonId::PieceBrick,
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Free, 1, true),
		},
		{
			TEXT("Free placement changes the placement and nothing else"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Snap, 2, true),
			EToolbarButtonId::PlacementFree,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 2, true),
		},
		{
			TEXT("Snap placement changes the placement and nothing else"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 2, true),
			EToolbarButtonId::PlacementSnap,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Snap, 2, true),
		},
		{
			TEXT("up a course from the ground keeps the piece and the placement"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 0, true),
			EToolbarButtonId::CourseUp,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 1, true),
		},
		{
			TEXT("down a course from two"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Snap, 2, false),
			EToolbarButtonId::CourseDown,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Snap, 1, false),
		},
		{
			TEXT("down a course from one lands on the grounded course"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 1, false),
			EToolbarButtonId::CourseDown,
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 0, false),
		},
		{
			TEXT("THE FLOOR: down a course from the grounded one is refused outright, not clamped into a new state"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 0, true),
			EToolbarButtonId::CourseDown,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 0, true),
		},
		{
			TEXT("Clear is a command the controller runs; the toolbar's own state is untouched by it"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 3, true),
			EToolbarButtonId::ClearBuild,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 3, true),
		},
		{
			TEXT("and Clear with nothing built is the same untouched state"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 3, false),
			EToolbarButtonId::ClearBuild,
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 3, false),
		},
		{
			TEXT("Run is a command too, and in Destroy mode it still changes no state"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 5, true),
			EToolbarButtonId::RunStructure,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 5, true),
		},
		{
			TEXT("Run is not on the strip in Build mode, so a click carrying it does nothing"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true),
			EToolbarButtonId::RunStructure,
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true),
		},
		{
			TEXT("a piece button is not on the strip in Destroy mode, so it must not quietly change the piece"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true),
			EToolbarButtonId::PieceTimberPlate,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true),
		},
		{
			TEXT("nor a placement button"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true),
			EToolbarButtonId::PlacementFree,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true),
		},
		{
			TEXT("nor the course buttons, in either direction"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 3, true),
			EToolbarButtonId::CourseUp,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 3, true),
		},
		{
			TEXT("nor CourseDown, which would otherwise be perfectly legal at course 3"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 3, true),
			EToolbarButtonId::CourseDown,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 3, true),
		},
	};

	for (const FTransitionCase& Case : Cases)
	{
		const FSessionToolbarState After = ApplyToolbarButton(Case.Before, Case.Click);

		TestTrue(
			*FString::Printf(
				TEXT("%s: %s + %s must give %s, it gave %s"),
				Case.Description, *DescribeState(Case.Before), NameOfButton(Case.Click),
				*DescribeState(Case.Expected), *DescribeState(After)),
			StatesEqual(After, Case.Expected));
	}

	/*
	 * THE TWO FUNCTIONS HELD AGAINST EACH OTHER, over every state and every button in the
	 * vocabulary — including the buttons that state does not draw. This is the property the table
	 * above cannot cover by enumeration: 72 states times 11 buttons is 792 clicks, and what it
	 * asserts is one-directional on purpose. A button that is absent or greyed MUST leave the state
	 * alone; nothing is claimed here about the ones that are lit and enabled, because that is what
	 * the table is for.
	 */
	for (const FSessionToolbarState& State : AllStates())
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);

		for (EToolbarButtonId Id : AllButtonIds())
		{
			const FToolbarButton* Drawn = FindButton(Buttons, Id);
			const bool bActionable = Drawn != nullptr && Drawn->bEnabled;
			const FSessionToolbarState After = ApplyToolbarButton(State, Id);

			if (!bActionable)
			{
				TestTrue(
					*FString::Printf(
						TEXT("%s: %s is %s, so clicking it must change nothing — it gave %s"),
						*DescribeState(State), NameOfButton(Id),
						Drawn == nullptr ? TEXT("not on the strip") : TEXT("greyed out"),
						*DescribeState(After)),
					StatesEqual(After, State));
			}

			/*
			 * AND NO CLICK MAY EVER PRODUCE A NEGATIVE COURSE. The struct says "never negative" and
			 * this is the only door into it; a plane below the ground is a readout nobody can make
			 * sense of and a build plane nothing can rest on.
			 */
			TestTrue(
				*FString::Printf(
					TEXT("%s: %s must never leave the course negative, it gave %s"),
					*DescribeState(State), NameOfButton(Id), *DescribeState(After)),
				After.Course >= 0);
		}
	}

	return true;
}

/**
 * THE PALETTE: EACH PIECE KIND IS ONE HALF-EXTENT AND ONE MATERIAL, AND BOTH ARE THE ONES THE
 * ALREADY-BUILT DEMO USES.
 *
 * THE SIZES ARE NOT NEW GEOMETRY. The brick is the standard 21.5 x 10.25 x 6.5 unit this whole
 * project is calibrated on, halved; the plate is Core/BuildMode/DemoBuilding.cpp's own wall plate
 * (33.75, 5.125, 5.0) transcribed; the lintel is that plate's 90 cm sibling. Pinning them here is
 * what stops the toolbar becoming a THIRD place brick dimensions are written down.
 *
 * THE MATERIAL IS COMPARED BY ADDRESS, NEVER BY VALUE. Two profiles with equal fields are equal in
 * every way except the one that matters: which library row a future retune moves. A by-value
 * comparison would go on passing against a private copy of Timber that the mean-strength re-anchor
 * never reached, and every joint the snap solver infers off that piece would be inferred from stale
 * numbers. It is also the same identity rule PieceActionsFor keeps for its action rows.
 *
 * THE UNKNOWN-KIND ROW IS THE FAIL-CLOSED ONE. EBuildPieceKind is a uint8 and a cast is all it takes
 * to produce a value nobody declared; what may not happen is a NaN or negative extent (a piece whose
 * mass is not a number) or a reference to something that is not a library row at all. This claim is
 * deliberately loose about WHICH answer an unknown kind gets — that is a decision this test is not
 * in a position to make — and strict about the answer being a real one.
 *
 * NEEDS A TICKING WORLD: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarPaletteTest,
	"DestructionGame.Core.SessionToolbar.Palette",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarPaletteTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	struct FPaletteCase
	{
		const TCHAR* Description;
		EBuildPieceKind Kind;
		FVector ExpectedHalfExtentCm;
		const DestructionProfiles::FMaterialProfile* ExpectedMaterial;
	};

	const FPaletteCase Cases[] = {
		{
			TEXT("the standard 21.5 x 10.25 x 6.5 cm clay brick, halved"),
			EBuildPieceKind::Brick,
			FVector(10.75, 5.125, 3.25),
			&DestructionProfiles::ClayBrick,
		},
		{
			TEXT("the demo building's own 67.5 cm timber wall plate"),
			EBuildPieceKind::TimberPlate,
			FVector(33.75, 5.125, 5.0),
			&DestructionProfiles::Timber,
		},
		{
			TEXT("a 90 cm timber lintel — the plate's longer sibling, for spanning an opening"),
			EBuildPieceKind::TimberLintel,
			FVector(45.0, 5.125, 5.0),
			&DestructionProfiles::Timber,
		},
	};

	for (const FPaletteCase& Case : Cases)
	{
		const FVector HalfExtentCm = BuildPieceHalfExtentCm(Case.Kind);

		TestTrue(
			*FString::Printf(
				TEXT("%s (%s): the half extent must be %s cm, it is %s cm"),
				Case.Description, NameOfPiece(Case.Kind),
				*DescribeVector(Case.ExpectedHalfExtentCm), *DescribeVector(HalfExtentCm)),
			VectorsExactlyEqual(HalfExtentCm, Case.ExpectedHalfExtentCm));

		const DestructionProfiles::FMaterialProfile& Material = BuildPieceMaterial(Case.Kind);

		TestTrue(
			*FString::Printf(
				TEXT("%s (%s): the material must BE the library's %s row, by address — it is %s"),
				Case.Description, NameOfPiece(Case.Kind),
				NameOfMaterialByAddress(*Case.ExpectedMaterial), NameOfMaterialByAddress(Material)),
			&Material == Case.ExpectedMaterial);
	}

	/*
	 * A KIND THIS BUILD HAS NEVER HEARD OF. Neither answer may be garbage: an extent that is not a
	 * number becomes a mass that is not a number two calls later, and a reference to something
	 * outside the library is a dangling read dressed as a material.
	 */
	const EBuildPieceKind UnknownKind = static_cast<EBuildPieceKind>(200);
	const FVector UnknownExtentCm = BuildPieceHalfExtentCm(UnknownKind);

	TestTrue(
		*FString::Printf(
			TEXT("a piece kind this build has never heard of must still answer with a real extent, it answered %s cm"),
			*DescribeVector(UnknownExtentCm)),
		UnknownExtentCm.IsZero()
			|| (!UnknownExtentCm.ContainsNaN()
				&& UnknownExtentCm.X > 0.0 && UnknownExtentCm.Y > 0.0 && UnknownExtentCm.Z > 0.0));

	TestTrue(
		*FString::Printf(
			TEXT("a piece kind this build has never heard of must answer with a shipped library row, it answered %s"),
			NameOfMaterialByAddress(BuildPieceMaterial(UnknownKind))),
		IsAShippedProfile(BuildPieceMaterial(UnknownKind)));

	return true;
}

/**
 * THE BUILD PLANE OF A COURSE PUTS THE PIECE ON TOP OF THE GROUND, NOT HALF INSIDE IT: COURSE N'S
 * BOTTOM IS N BRICK COURSES UP AND THE PLANE IS THE PIECE'S OWN HALF-HEIGHT ABOVE THAT.
 *
 * THIS IS THE SLICE'S ONE RULING AND IT IS A CONVENTION CHANGE. Every harness this project has built
 * so far centres course 0 at Z = 0, which puts the grounded course half below the ground plane — a
 * render follow-up already records the picture that produces. A player placing the first brick of
 * their own building must see it sitting ON the ground, so the model answers 3.25 for a brick on
 * course 0 rather than 0. Nothing about the STRUCTURE changes: the solver reads relative positions
 * only, so this is the same building lifted by one brick half-height, which the integration claim
 * in this file measures rather than asserts by hand.
 *
 * THE PITCH IS THE BRICK'S, WHATEVER THE PIECE IS. 7.5 cm is 6.5 of brick plus a 1 cm bed joint, and
 * a course is a property of the WALL rather than of the thing being laid into it — a timber plate on
 * course 2 bears on two brick courses and their joints, which is exactly where the demo building
 * puts its own plate. A pitch derived from the piece would put the plate at its own doubled height
 * and the bearing would be imaginary.
 *
 * THE TEST SPELLS 7.5 OUT AND ALSO DERIVES IT. Production is required to read FSnapSettings rather
 * than hard-code the sum; this file asserts the sum against a default FSnapSettings SEPARATELY from
 * the plane, so a retune of the brick or the joint thickness fails on a row that names the retune
 * rather than silently agreeing with whatever the plane now returns. That is the DESIGN §3 rule
 * about conversion constants applied to a coordinating dimension.
 *
 * A NEGATIVE COURSE IS TREATED AS COURSE 0 — the fail-closed row, and the reason the state's own
 * comment says the course is never negative. The plane is a build height AND a player-facing
 * readout; below-ground is neither, and FMath::Max on an already-signed input is precisely the
 * spelling that turns a wrong course into a plausible height.
 *
 * NEEDS A TICKING WORLD: no. Two numbers in, one number out.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarCoursePlaneZTest,
	"DestructionGame.Core.SessionToolbar.CoursePlaneZ",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarCoursePlaneZTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	/*
	 * THE PITCH, DERIVED FROM THE SNAP SETTINGS THE SOLVER ALREADY USES. If this row fails, the
	 * brick or the joint has been retuned and every expected height below is stale — which is the
	 * whole reason it is asserted before them rather than folded into them.
	 */
	const BuildMode::FSnapSettings Settings;

	TestEqual(
		FString::Printf(
			TEXT("the brick course pitch must be %g cm (%g of brick + %g of joint), the settings give %g"),
			BrickCoursePitchCm, Settings.BrickSizeCm.Z, Settings.JointThicknessCm,
			Settings.BrickSizeCm.Z + Settings.JointThicknessCm),
		Settings.BrickSizeCm.Z + Settings.JointThicknessCm, BrickCoursePitchCm);

	TestEqual(
		FString::Printf(
			TEXT("the brick's half height must be %g cm, the settings give %g"),
			BrickHalfHeightCm, 0.5 * Settings.BrickSizeCm.Z),
		0.5 * Settings.BrickSizeCm.Z, BrickHalfHeightCm);

	struct FPlaneCase
	{
		const TCHAR* Description;
		int32 Course;
		double PieceHalfHeightCm;
		double ExpectedZCm;
	};

	const FPlaneCase Cases[] = {
		{
			TEXT("a brick on the grounded course rests ON the ground, not half in it"),
			0, BrickHalfHeightCm, 3.25,
		},
		{
			TEXT("the next course up: its bottom is one 7.5 cm pitch above the ground"),
			1, BrickHalfHeightCm, 10.75,
		},
		{
			TEXT("a timber plate on course 2 bears on two brick courses and their joints"),
			2, PlateHalfHeightCm, 20.0,
		},
		{
			TEXT("a brick five courses up"),
			5, BrickHalfHeightCm, 40.75,
		},
		{
			TEXT("FAIL CLOSED: a negative course is course 0, for a brick"),
			-1, BrickHalfHeightCm, 3.25,
		},
		{
			TEXT("FAIL CLOSED: and a very negative one, for a plate"),
			-7, PlateHalfHeightCm, 5.0,
		},
	};

	for (const FPlaneCase& Case : Cases)
	{
		const double ZCm = CoursePlaneZCm(Case.Course, Case.PieceHalfHeightCm);

		TestEqual(
			FString::Printf(
				TEXT("%s: course %d with a half height of %g cm must put the build plane at %g cm, it is at %g cm"),
				Case.Description, Case.Course, Case.PieceHalfHeightCm, Case.ExpectedZCm, ZCm),
			ZCm, Case.ExpectedZCm);
	}

	/*
	 * AND THE LADDER CLIMBS BY EXACTLY ONE PITCH PER COURSE, for both half-heights. The rows above
	 * pin three points; this pins the step between every pair of them, which is what makes an
	 * off-by-one-joint pitch — 6.5 instead of 7.5, the same mistake as forgetting the mortar — fail
	 * on the step rather than on a single arithmetic coincidence.
	 */
	const double HalfHeights[] = { BrickHalfHeightCm, PlateHalfHeightCm };

	for (double HalfHeightCm : HalfHeights)
	{
		for (int32 Course = 0; Course < 8; ++Course)
		{
			const double Lower = CoursePlaneZCm(Course, HalfHeightCm);
			const double Upper = CoursePlaneZCm(Course + 1, HalfHeightCm);

			TestEqual(
				FString::Printf(
					TEXT("a half height of %g cm: course %d to %d must climb exactly one %g cm pitch, it climbed %g cm"),
					HalfHeightCm, Course, Course + 1, BrickCoursePitchCm, Upper - Lower),
				Upper - Lower, BrickCoursePitchCm);

			TestTrue(
				*FString::Printf(
					TEXT("a half height of %g cm: the plane at course %d must be a number, it is %g"),
					HalfHeightCm, Course, Lower),
				FMath::IsFinite(Lower));
		}
	}

	return true;
}

/**
 * COURSE 0 IS THE ONE THAT TOUCHES THE EARTH, AND THE COURSE READOUT NAMES THE COURSE IT IS SHOWING.
 *
 * GROUNDED IS THE FLAG FStructure ROUTES LOAD TO, so "which course is grounded" is not a cosmetic
 * question — a piece laid with the flag set is a piece that absorbs whatever reaches it, and a whole
 * building marked grounded is a building that cannot fall. One course has the earth under it and
 * that is course 0; everything above is held up by what is below it or by nothing.
 *
 * THE LABEL IS THE MODEL'S FOR THE REASON EVERY OTHER CAPTION HERE IS, and the claim is bounded the
 * same way: it must contain the course number, it must never be blank, and two different courses
 * must never read the same — a readout that says the same thing on every course is a readout that
 * is not reading anything.
 *
 * AND THE NUMBER IT PRINTS COUNTS FROM ONE, WHICH IS NOT THE NUMBER IT IS STORING — OWNER-DELEGATED
 * RULING, 2026-09-15 (CURRENT_STATE finding P1). The session has TWO surfaces that name courses and
 * they disagreed: this strip printed the grounded course as "Course 0" while the piece menu's own
 * readout counts from one ("course 1 · #2" for a brick laid on that very plane, Core/PieceMenu.cpp,
 * "BOTH NUMBERS COUNT FROM ONE"). The session proof frames are what made it a defect rather than a
 * quibble — a player lays a brick on the strip's "Course 0" and the details window calls the brick
 * they just laid "course 1". One of the two had to move, and the one that moves is THIS one: a
 * person counting courses of brick starts at one and always has, and the readout the player spends
 * longest reading is the one naming individual bricks.
 *
 * FSessionToolbarState::Course STAYS ZERO-BASED, and that separation is the whole of the change.
 * CoursePlaneZCm, IsCourseGrounded and the stepper's floor are arithmetic over an index and are
 * untouched — course 0 is still the one with the earth under it and still planes at one half-height.
 * What moves is the one function that turns that index into words, which is why this is a label
 * change with no physical consequence and why the grounding rows above it are unchanged.
 *
 * A NEGATIVE COURSE ANSWERS AS COURSE 0 THROUGHOUT. The clamp is a property of the whole course
 * vocabulary rather than of one function: a below-ground course that reads "not grounded" on one
 * call and gets a course-0 build plane on the next is two functions disagreeing about a state that
 * is not supposed to exist, and that disagreement is what would make it survive.
 *
 * NEEDS A TICKING WORLD: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarCourseGroundingAndLabelTest,
	"DestructionGame.Core.SessionToolbar.CourseGroundingAndLabel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarCourseGroundingAndLabelTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	TestTrue(
		TEXT("course 0 sits on the earth and must read as grounded"),
		IsCourseGrounded(0));

	TestFalse(
		TEXT("course 1 is held up by course 0, not by the earth"),
		IsCourseGrounded(1));

	TestFalse(
		TEXT("course 5 is nowhere near the earth"),
		IsCourseGrounded(5));

	TestTrue(
		TEXT("FAIL CLOSED: a negative course answers as course 0 does — grounded"),
		IsCourseGrounded(-1));

	/*
	 * THE TWO EXACT PINS, AND THEY ARE EXACT BECAUSE THE WHOLE CLAIM IS ONE OF AGREEMENT. A
	 * "contains its number" assertion is satisfied by either convention on most courses — "Course 1"
	 * contains a 1 whether it is the index or the index plus one — so the rows that separate the two
	 * conventions have to be spelled out in full.
	 */
	TestEqual(
		FString::Printf(
			TEXT("the GROUNDED course is the player's FIRST course: CourseLabel(0) must read 'Course 1', it reads '%s'"),
			*CourseLabel(0)),
		CourseLabel(0), FString(TEXT("Course 1")));

	TestEqual(
		FString::Printf(
			TEXT("and the count keeps step: CourseLabel(2) must read 'Course 3', it reads '%s'"),
			*CourseLabel(2)),
		CourseLabel(2), FString(TEXT("Course 3")));

	const FString LabelForThree = CourseLabel(3);

	TestTrue(
		*FString::Printf(
			TEXT("the course readout must name its course ONE-BASED: CourseLabel(3) reads '%s' and must contain '4'"),
			*LabelForThree),
		LabelForThree.Contains(TEXT("4")));

	/*
	 * AND MUST NOT STILL PRINT THE INDEX. Without this row a label reading "Course 3 (index 3)" —
	 * or an implementation that printed both numbers to keep everyone happy — would satisfy the
	 * row above it, and the player would be back to reading two different numbers for one course.
	 */
	TestFalse(
		*FString::Printf(
			TEXT("and must NOT print the zero-based index beside it: CourseLabel(3) reads '%s' and must not contain '3'"),
			*LabelForThree),
		LabelForThree.Contains(TEXT("3")));

	TSet<FString> SeenLabels;

	for (int32 Course = 0; Course <= 5; ++Course)
	{
		const FString Label = CourseLabel(Course);

		TestFalse(
			*FString::Printf(TEXT("the course readout must never be blank, course %d reads nothing"), Course),
			Label.IsEmpty());

		TestTrue(
			*FString::Printf(
				TEXT("the course readout must contain its own ONE-BASED number: index %d must read '%d', it reads '%s'"),
				Course, Course + 1, *Label),
			Label.Contains(FString::FromInt(Course + 1)));

		bool bAlreadyThere = false;
		SeenLabels.Add(Label, &bAlreadyThere);

		TestFalse(
			*FString::Printf(
				TEXT("two different courses must not read the same: course %d also reads '%s'"),
				Course, *Label),
			bAlreadyThere);
	}

	TestEqual(
		FString::Printf(
			TEXT("FAIL CLOSED: a negative course must read as course 0 ('%s'), it reads '%s'"),
			*CourseLabel(0), *CourseLabel(-2)),
		CourseLabel(-2), CourseLabel(0));

	TestEqual(
		FString::Printf(
			TEXT("FAIL CLOSED, AND IT IS THE PLAYER'S FIRST COURSE IT FALLS BACK TO: CourseLabel(-1) must read 'Course 1', it reads '%s'"),
			*CourseLabel(-1)),
		CourseLabel(-1), FString(TEXT("Course 1")));

	return true;
}

/**
 * WALKING THE DEMO BUILDING THROUGH THE MODEL REPRODUCES IT EXACTLY, LIFTED BY ONE BRICK
 * HALF-HEIGHT — THE SAME BUILDING, RESTING ON THE GROUND INSTEAD OF STRADDLING IT.
 *
 * WHY THIS IS THE INTEGRATION CLAIM AND WHY IT IS STILL WORLD-FREE. The toolbar model is not a new
 * way to describe buildings: it is a presenter over the snap solver and the placement API that
 * Core/BuildMode/DemoBuilding.cpp already drives, and the way to know it has not invented its own
 * geometry is to point it at the one building this project has already built and pinned. Every
 * number on the demo's side is READ OUT OF THE BUILDING ITSELF — BuildDemoBuilding is run into an
 * FBrickLayout here and the placed centres and extents are taken off Layout.Boxes — so if the
 * palette or the plane drifts away from what the demo uses, the two stop agreeing here rather than
 * in a screenshot somebody looks at later.
 *
 * READ, NOT TRANSCRIBED, AND THAT IS THE WHOLE POINT OF THE INDIRECTION. Three copied literals
 * (0, 7.5, 16.75) would keep passing after the demo migrated onto this very convention, because
 * they would still describe the building as it used to be. Reading the layout makes this test go
 * red AT the migration — which is exactly when somebody needs to be told that the lift is now zero
 * and this claim has been discharged — and not one commit before.
 *
 * THE ASSERTION IS THE OFFSET BEING THE SAME ON EVERY COURSE, not that any one height matches. A
 * model that put the plate at 20.0 by luck and the bricks somewhere else would satisfy a single
 * height check and would be a different building; a CONSTANT offset is the whole content of the
 * ruling — "same relative geometry, shifted to rest on the ground" — and it is what guarantees the
 * solver reads the identical structure, since the solver reads relative positions only.
 *
 * AND THE OFFSET IS NOT AN ARBITRARY CONSTANT. It must equal the brick's own half-height, because
 * that is what "course 0 centred at 0 becomes course 0 resting on the ground" means. Pinning it as a
 * free 3.25 would be pinning a coincidence.
 *
 * NEEDS A TICKING WORLD: no. The claim is about two sets of numbers, and the world-side proof that
 * the lifted building still stands belongs with the builder, not here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarDemoBuildingHeightsTest,
	"DestructionGame.Core.SessionToolbar.DemoBuildingHeights",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarDemoBuildingHeightsTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	const FVector PlateHalfExtentCm = BuildPieceHalfExtentCm(EBuildPieceKind::TimberPlate);
	const FVector BrickHalfExtentCm = BuildPieceHalfExtentCm(EBuildPieceKind::Brick);

	/*
	 * THE DEMO BUILDING, BUILT. World-free: BuildDemoBuilding grows an FBrickLayout through
	 * BuildMode::PlacePiece, so the boxes it leaves behind are the poses the snap solver actually
	 * committed — not a description of them.
	 */
	DestructionLayout::FBrickLayout DemoLayout;
	BuildMode::BuildDemoBuilding(DemoLayout, BuildMode::FSnapSettings{});

	if (DemoLayout.Boxes.Num() <= DemoPlatePieceIndex)
	{
		AddError(FString::Printf(
			TEXT("fixture: the demo building laid only %d boxes, too few to read its plate at index %d"),
			DemoLayout.Boxes.Num(), DemoPlatePieceIndex));
		return true;
	}

	const DestructionLayout::FPieceBox& DemoPlateBox = DemoLayout.Boxes[DemoPlatePieceIndex];

	/* The palette's plate IS the demo's plate, or the heights below are about a different board. */
	TestTrue(
		*FString::Printf(
			TEXT("the palette's plate must be the board the demo actually lays, which is %s cm; the palette's is %s cm"),
			*DescribeVector(DemoPlateBox.ExtentCm), *DescribeVector(PlateHalfExtentCm)),
		VectorsExactlyEqual(PlateHalfExtentCm, DemoPlateBox.ExtentCm));

	const double PlateZCm = CoursePlaneZCm(2, PlateHalfExtentCm.Z);

	TestEqual(
		FString::Printf(
			TEXT("the plate on course 2 must sit at 20 cm, it sits at %g cm"), PlateZCm),
		PlateZCm, 20.0);

	struct FLiftCase
	{
		const TCHAR* Description;
		int32 Course;
		double HalfHeightCm;
		double DemoCentreZCm;
	};

	const FLiftCase Cases[] = {
		{ TEXT("the grounded brick course"), 0, BrickHalfHeightCm,
			DemoLayout.Boxes[DemoCourseZeroPieceIndex].CentreCm.Z },
		{ TEXT("the staggered course above it"), 1, BrickHalfHeightCm,
			DemoLayout.Boxes[DemoCourseOnePieceIndex].CentreCm.Z },
		{ TEXT("the timber wall plate bearing across the top"), 2, PlateHalfExtentCm.Z,
			DemoPlateBox.CentreCm.Z },
	};

	for (const FLiftCase& Case : Cases)
	{
		const double ModelZCm = CoursePlaneZCm(Case.Course, Case.HalfHeightCm);
		const double LiftCm = ModelZCm - Case.DemoCentreZCm;

		TestEqual(
			FString::Printf(
				TEXT("%s: the model puts it at %g cm and the demo at %g cm, so the lift must be exactly one brick half-height (%g cm), it is %g cm"),
				Case.Description, ModelZCm, Case.DemoCentreZCm, BrickHalfHeightCm, LiftCm),
			LiftCm, BrickHalfHeightCm);
	}

	/* And that lift is the brick's own half-height rather than a number that happens to be 3.25. */
	TestEqual(
		FString::Printf(
			TEXT("the ground offset must BE the palette brick's half height, which is %g cm"),
			BrickHalfExtentCm.Z),
		BrickHalfExtentCm.Z, BrickHalfHeightCm);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
