// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystemInterface.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputTriggers.h"
#include "RequiredContent.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named namespace, and named differently from every other one in this module — an
 * anonymous namespace is private to a translation unit rather than to a file, and a
 * unity build merges many files into one. This is not HoverInputBindingTestSupport or
 * InspectPieceBindingTestSupport, the two files this one is modelled on, and every
 * name in it carries a SessionAssets prefix so none can be ambiguous against theirs.
 * See CURRENT_STATE.md; the `using namespace` lives inside each RunTest for the same
 * reason.
 */
namespace SessionAssetsTestSupport
{
	/*
	 * The paths are spelled here rather than imported from RequiredContent.h, and
	 * that is the one place this file deliberately duplicates a string.
	 *
	 * RequiredContent.h's own header says there is no second copy of these paths in
	 * the module, and that rule is about production: one spelling is what stops a
	 * constructor and the table becoming two lists that disagree. A test is the other
	 * side of that — these assertions have to be able to go red because the asset is
	 * missing, and a test written against a constant that does not exist yet fails to
	 * compile instead, a build break rather than a statement about content, saying
	 * nothing on the day somebody renames the asset and the constant together.
	 *
	 * What keeps the two spellings honest is Content.RequiredAssetsResolve, which
	 * sweeps the table and the CDOs in both directions: a path constant that names
	 * nothing leaves the controller's FObjectFinder holding null and fails there, and
	 * an asset resolved onto the CDO with no table row fails there too. So this file
	 * does not duplicate that claim, and must not.
	 */
	constexpr const TCHAR* SessionLookModifierActionPath =
		TEXT("/Game/Input/Actions/IA_LookModifier.IA_LookModifier");

	constexpr const TCHAR* SessionToggleModeActionPath =
		TEXT("/Game/Input/Actions/IA_SessionToggleMode.IA_SessionToggleMode");

	constexpr const TCHAR* SessionPieceBrickActionPath =
		TEXT("/Game/Input/Actions/IA_SessionPieceBrick.IA_SessionPieceBrick");

	constexpr const TCHAR* SessionPiecePlateActionPath =
		TEXT("/Game/Input/Actions/IA_SessionPiecePlate.IA_SessionPiecePlate");

	constexpr const TCHAR* SessionPieceLintelActionPath =
		TEXT("/Game/Input/Actions/IA_SessionPieceLintel.IA_SessionPieceLintel");

	constexpr const TCHAR* SessionSnapToggleActionPath =
		TEXT("/Game/Input/Actions/IA_SessionSnapToggle.IA_SessionSnapToggle");

	constexpr const TCHAR* SessionCourseUpActionPath =
		TEXT("/Game/Input/Actions/IA_SessionCourseUp.IA_SessionCourseUp");

	constexpr const TCHAR* SessionCourseDownActionPath =
		TEXT("/Game/Input/Actions/IA_SessionCourseDown.IA_SessionCourseDown");

	constexpr const TCHAR* SessionRunActionPath =
		TEXT("/Game/Input/Actions/IA_SessionRun.IA_SessionRun");

	constexpr const TCHAR* SessionMappingContextPath =
		TEXT("/Game/Input/IMC_Session.IMC_Session");

	/** One row of the shortcut table: the action asset, the key it is reached by, and why. */
	struct FSessionShortcutRow
	{
		const TCHAR* ActionPath = nullptr;
		FKey Key;
		const TCHAR* What = nullptr;
	};

	/**
	 * The whole session keyboard, as data, so adding a shortcut is adding a row.
	 *
	 * The keys are SESSION_UI_DESIGN §b's Shortcut columns transcribed, and every one
	 * is a decision with a reason recorded there: `Tab` toggles the mode pair because
	 * there are exactly two of them and a toggle is one binding rather than two; `G`
	 * is grid rather than snap because `S` is strafe-back on the flying pawn; `[` and
	 * `]` are the course stepper because they read as a pair; `Enter` is the one
	 * command on the Destroy strip. The right mouse button is not a shortcut at all —
	 * it is the modifier the camera look is chorded to, the whole of §d.
	 */
	TArray<FSessionShortcutRow> SessionShortcutRows()
	{
		return {
			{ SessionLookModifierActionPath, EKeys::RightMouseButton, TEXT("hold to look around") },
			{ SessionToggleModeActionPath,   EKeys::Tab,              TEXT("toggle Build/Destroy") },
			{ SessionPieceBrickActionPath,   EKeys::One,              TEXT("palette: brick") },
			{ SessionPiecePlateActionPath,   EKeys::Two,              TEXT("palette: timber plate") },
			{ SessionPieceLintelActionPath,  EKeys::Three,            TEXT("palette: timber lintel") },
			{ SessionSnapToggleActionPath,   EKeys::G,                TEXT("toggle Snap/Free") },
			{ SessionCourseUpActionPath,     EKeys::RightBracket,     TEXT("course up") },
			{ SessionCourseDownActionPath,   EKeys::LeftBracket,      TEXT("course down") },
			{ SessionRunActionPath,          EKeys::Enter,            TEXT("run the structure") }
		};
	}

	/** Every mapping in a context, printed, so the log records what was authored at the time. */
	FString DescribeSessionContext(const UInputMappingContext* Context)
	{
		if (Context == nullptr)
		{
			return TEXT("<null context>");
		}

		FString Line;

		for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings())
		{
			Line += FString::Printf(
				TEXT("%s%s=%s"),
				Line.IsEmpty() ? TEXT("") : TEXT(", "),
				*GetNameSafe(Mapping.Action.Get()),
				*Mapping.Key.ToString());
		}

		return Line.IsEmpty() ? TEXT("<no mappings>") : Line;
	}

	/** Every trigger on a mapping, printed, including what a chord names. */
	FString DescribeSessionTriggers(const FEnhancedActionKeyMapping& Mapping)
	{
		FString Line;

		for (const TObjectPtr<UInputTrigger>& Trigger : Mapping.Triggers)
		{
			const UInputTriggerChordAction* const Chord = Cast<UInputTriggerChordAction>(Trigger);

			Line += FString::Printf(
				TEXT("%s%s%s"),
				Line.IsEmpty() ? TEXT("") : TEXT(", "),
				*GetNameSafe(Trigger),
				Chord != nullptr
					? *FString::Printf(TEXT("(chord=%s)"), *GetNameSafe(Chord->ChordAction))
					: TEXT(""));
		}

		return Line.IsEmpty() ? TEXT("<no triggers>") : Line;
	}

	/** How many chord triggers a mapping carries, and the one they name. */
	int32 CountSessionChords(const FEnhancedActionKeyMapping& Mapping, const UInputAction*& OutChorded)
	{
		int32 Count = 0;

		for (const TObjectPtr<UInputTrigger>& Trigger : Mapping.Triggers)
		{
			if (const UInputTriggerChordAction* const Chord = Cast<UInputTriggerChordAction>(Trigger))
			{
				++Count;
				OutChorded = Chord->ChordAction;
			}
		}

		return Count;
	}

	/** Every action-event binding on a component, printed, so the log records what was bound. */
	FString DescribeSessionActionBindings(const UEnhancedInputComponent* Component)
	{
		if (Component == nullptr)
		{
			return TEXT("<no enhanced input component>");
		}

		FString Line;

		for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding
			: Component->GetActionEventBindings())
		{
			if (!Binding.IsValid())
			{
				continue;
			}

			Line += FString::Printf(
				TEXT("%s%s@%s"),
				Line.IsEmpty() ? TEXT("") : TEXT(", "),
				*GetNameSafe(Binding->GetAction()),
				*UE::Input::LexToString(Binding->GetTriggerEvent()));
		}

		return Line.IsEmpty() ? TEXT("<no action bindings>") : Line;
	}

	/**
	 * The eight shortcuts that reach a handler — the nine rows above minus the look
	 * modifier.
	 *
	 * The modifier is not one of them, the point of splitting the table.
	 * IA_LookModifier does nothing on its own: it exists only to be the thing
	 * IMC_MouseLook's chord watches, so there is no handler for it to reach and a C++
	 * binding on it would be dead code. Every other row is one line onto
	 * OnToolbarButton.
	 */
	TArray<FSessionShortcutRow> SessionBoundShortcutRows()
	{
		TArray<FSessionShortcutRow> Rows = SessionShortcutRows();

		Rows.RemoveAll(
			[](const FSessionShortcutRow& Row)
			{
				return FCString::Strcmp(Row.ActionPath, SessionLookModifierActionPath) == 0;
			});

		return Rows;
	}

	/**
	 * The rebuilt per-player mapping list, in the order a frame will evaluate it.
	 *
	 * Indexed, because the index is the claim below — a list printed without them
	 * would record the order and still make a reader count. The triggers come with
	 * it so a chord is visible in the log beside the mapping it gates.
	 */
	FString DescribeSessionMappingOrder(TConstArrayView<const FEnhancedActionKeyMapping> Mappings)
	{
		FString Line;

		for (int32 Index = 0; Index < Mappings.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s[%d] %s<-%s%s"),
				Line.IsEmpty() ? TEXT("") : TEXT(", "),
				Index,
				*GetNameSafe(Mappings[Index].Action.Get()),
				*Mappings[Index].Key.ToString(),
				Mappings[Index].Triggers.Num() > 0
					? *FString::Printf(TEXT(" {%s}"), *DescribeSessionTriggers(Mappings[Index]))
					: TEXT(""));
		}

		return Line.IsEmpty() ? TEXT("<no mappings at all>") : Line;
	}

	/**
	 * Where this exact action-on-this-exact-key sits in that list, or INDEX_NONE.
	 *
	 * BOTH HALVES, because either alone would find the wrong row: IA_MouseLook could one day be
	 * mapped to a second key, and Mouse2D already carries IA_HoverPiece as well.
	 */
	int32 IndexOfSessionMapping(
		TConstArrayView<const FEnhancedActionKeyMapping> Mappings,
		const UInputAction* Action,
		const FKey& Key)
	{
		for (int32 Index = 0; Index < Mappings.Num(); ++Index)
		{
			if (Mappings[Index].Action.Get() == Action && Mappings[Index].Key == Key)
			{
				return Index;
			}
		}

		return INDEX_NONE;
	}

	/**
	 * A floor on what IMC_Default already maps, so the key-collision sweep below
	 * cannot pass by sweeping over nothing. Measured off the asset: eight mappings
	 * before the piece menu's two landed. Six is comfortably under that and well over
	 * zero, the only thing this number has to be — the same floor and reasoning as
	 * Tests/InspectPieceBindingTest.cpp's.
	 */
	constexpr int32 SessionDefaultMappingFloor = 6;

	/** The nine mappings IMC_Session is authored with, and exactly nine. */
	constexpr int32 SessionMappingCount = 9;
}

/**
 * S6 — the session's own input context exists, and it carries one digital action per
 * shortcut the toolbar draws, on the key the design names.
 *
 * BEHAVIOUR: `/Game/Input/IMC_Session` maps nine boolean input actions — the look
 * modifier, the mode toggle, three piece chips, the snap toggle, the two course steps
 * and Run — each to exactly the key SESSION_UI_DESIGN §b's Shortcut columns name.
 *
 * WHY THIS IS A TEST AND NOT JUST AN ASSET. The same argument
 * Tests/InspectPieceBindingTest.cpp opens with, and it has grown teeth since: a
 * mapping authored by hand and never asserted is a mapping that silently stops
 * existing the next time the asset is re-saved, and the symptom here is not a crash
 * but a key that does nothing. Nine of them at once is nine ways for a session to feel
 * broken with nothing in the log.
 *
 * A table rather than nine assertions, for the reason this project prefers one
 * parameterised test to a row per case everywhere else: adding `L`, `X` and `J` for
 * the Destroy settings (§b) must be adding three rows, not writing three tests.
 *
 * THE VALUE TYPE IS PART OF THE CLAIM. Every one of these is a press — a bool the
 * controller turns into one `OnToolbarButton` call. An action authored Axis1D or
 * Axis2D actuates on a value rather than a press, and Enhanced Input would then fire
 * the handler on frames the player is not asking for anything; the look modifier in
 * particular has to be a bool because a chord asks "is that action triggering", a
 * yes/no question.
 *
 * And the count is exact: nine, not "at least nine". A tenth mapping in this context
 * is a key doing something the strip does not draw, the one input in the game with no
 * on-screen affordance to explain it.
 *
 * Needs a ticking world: no. Ten assets loaded by path — no world, no player, no input
 * subsystem. Whether SetupInputComponent binds these actions is the separate question
 * in Tests/SessionShortcutTest.cpp, and whether the controller references them is
 * Content.RequiredAssetsResolve's.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionContextMapsTheShortcutsTest,
	"DestructionGame.Content.SessionInput.SessionContextMapsTheShortcuts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionContextMapsTheShortcutsTest::RunTest(const FString& Parameters)
{
	using namespace SessionAssetsTestSupport;

	const UInputMappingContext* const SessionContext = LoadObject<UInputMappingContext>(
		nullptr, SessionMappingContextPath);

	TestNotNull(
		*FString::Printf(
			TEXT("the session's keyboard needs a mapping context at '%s'; it does not resolve"),
			SessionMappingContextPath),
		SessionContext);

	if (SessionContext == nullptr)
	{
		return true;
	}

	AddInfo(FString::Printf(TEXT("IMC_Session maps: %s"), *DescribeSessionContext(SessionContext)));

	const TArray<FEnhancedActionKeyMapping>& Mappings = SessionContext->GetMappings();

	TestEqual(
		FString::Printf(
			TEXT("IMC_Session must carry exactly %d mappings — one per shortcut the strip draws, and "
				 "not one more: a key doing something no chip explains is an input a player cannot "
				 "discover. It carries %d [%s]"),
			SessionMappingCount, Mappings.Num(), *DescribeSessionContext(SessionContext)),
		Mappings.Num(), SessionMappingCount);

	for (const FSessionShortcutRow& Row : SessionShortcutRows())
	{
		const UInputAction* const Action = LoadObject<UInputAction>(nullptr, Row.ActionPath);

		TestNotNull(
			*FString::Printf(
				TEXT("'%s' (%s) must resolve to an input action; it does not"), Row.ActionPath, Row.What),
			Action);

		if (Action == nullptr)
		{
			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("'%s' is a PRESS, so it must be a Boolean action — an axis actuates on a value "
					 "and would fire the handler on frames the player asked for nothing. It is value "
					 "type %d against Boolean (%d)"),
				Row.ActionPath,
				static_cast<int32>(Action->ValueType),
				static_cast<int32>(EInputActionValueType::Boolean)),
			Action->ValueType == EInputActionValueType::Boolean);

		TArray<FKey> Keys;

		for (const FEnhancedActionKeyMapping& Mapping : Mappings)
		{
			if (Mapping.Action.Get() == Action)
			{
				Keys.Add(Mapping.Key);
			}
		}

		TestEqual(
			FString::Printf(
				TEXT("IMC_Session must map '%s' (%s) exactly once; it maps it %d time(s) [%s]"),
				Row.ActionPath, Row.What, Keys.Num(), *DescribeSessionContext(SessionContext)),
			Keys.Num(), 1);

		if (Keys.Num() != 1)
		{
			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("and on %s, which is what SESSION_UI_DESIGN §b draws on the chip for '%s'; it is "
					 "mapped to %s"),
				*Row.Key.ToString(), Row.What, *Keys[0].ToString()),
			Keys[0] == Row.Key);
	}

	return true;
}

/**
 * S6 — no session shortcut steals a key the player is flying with.
 *
 * BEHAVIOUR: not one key IMC_Session maps is a key IMC_Default already maps, so
 * applying the session's context alongside the flying pawn's takes nothing away from
 * `W A S D Q E Space` or from the piece click.
 *
 * WHY A KEY-SET DIFF RATHER THAN A LIST OF FORBIDDEN KEYS. `UInputAction::bConsumeInput`
 * defaults to true and a consumed key is withheld from every mapping below it in the
 * applied stack — so a session shortcut that lands on a key the pawn is using does not
 * merely double up, it takes the key away, and the symptom is a player who presses `S`
 * and stops flying backwards with nothing logged and nothing on screen. The hazard is
 * therefore about whatever IMC_Default happens to contain rather than about six
 * letters somebody wrote down, so the two assets are read and their key sets diffed —
 * it keeps meaning the same thing the day the pawn grows a sprint key.
 *
 * (`Tests/HoverInputBindingTest.cpp` makes the neighbouring claim from the other side
 * — hover shares Mouse2D with free-look deliberately, and so must not consume it. The
 * session's keys are not shared at all, the stronger position and why this asserts
 * disjointness rather than non-consumption.)
 *
 * The floor is what stops this passing over nothing: a context that failed to load,
 * or lost its mappings, would make a disjointness claim vacuously true — exactly the
 * state this test is supposed to be loudest about.
 *
 * Needs a ticking world: no. Two assets, loaded by path.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionKeysAreFreeTest,
	"DestructionGame.Content.SessionInput.SessionKeysAreFree",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionKeysAreFreeTest::RunTest(const FString& Parameters)
{
	using namespace SessionAssetsTestSupport;

	const UInputMappingContext* const SessionContext = LoadObject<UInputMappingContext>(
		nullptr, SessionMappingContextPath);

	const UInputMappingContext* const DefaultContext = LoadObject<UInputMappingContext>(
		nullptr, DestructionContent::DefaultMappingContextPath);

	TestNotNull(
		*FString::Printf(TEXT("'%s' must resolve"), SessionMappingContextPath),
		SessionContext);

	TestNotNull(
		*FString::Printf(TEXT("fixture: '%s' should resolve"),
			DestructionContent::DefaultMappingContextPath),
		DefaultContext);

	if (SessionContext == nullptr || DefaultContext == nullptr)
	{
		return true;
	}

	AddInfo(FString::Printf(TEXT("IMC_Session maps: %s"), *DescribeSessionContext(SessionContext)));
	AddInfo(FString::Printf(TEXT("IMC_Default maps: %s"), *DescribeSessionContext(DefaultContext)));

	TestTrue(
		FString::Printf(
			TEXT("fixture: IMC_Default should already carry at least %d mappings for a collision sweep "
				 "to mean anything; it carries %d"),
			SessionDefaultMappingFloor, DefaultContext->GetMappings().Num()),
		DefaultContext->GetMappings().Num() >= SessionDefaultMappingFloor);

	/*
	 * And IMC_Session carries its nine — exactly, not at least one. The sentence and
	 * the assertion have to be the same claim: reading `>= 1` while saying "its 9"
	 * would be a diffing sweep that stays green over a context that lost eight of its
	 * nine mappings — eight dead keys, and the one sweep positioned to notice
	 * reporting nothing. The count is a claim SessionContextMapsTheShortcuts makes
	 * about the asset; here it is the fixture that makes the disjointness below worth
	 * anything, so it is spelled out rather than taken on trust from a test that may
	 * or may not have run first.
	 */
	TestEqual(
		FString::Printf(
			TEXT("fixture: and IMC_Session should carry its %d; it carries %d"),
			SessionMappingCount, SessionContext->GetMappings().Num()),
		SessionContext->GetMappings().Num(), SessionMappingCount);

	for (const FEnhancedActionKeyMapping& SessionMapping : SessionContext->GetMappings())
	{
		for (const FEnhancedActionKeyMapping& DefaultMapping : DefaultContext->GetMappings())
		{
			if (DefaultMapping.Key != SessionMapping.Key)
			{
				continue;
			}

			AddError(FString::Printf(
				TEXT("IMC_Session maps %s to %s, and IMC_Default already maps that key to %s — "
					 "bConsumeInput defaults to true, so the session shortcut does not merely double "
					 "up, it WITHHOLDS the key from the mapping below it and the pawn silently stops "
					 "answering"),
				*SessionMapping.Key.ToString(),
				*GetNameSafe(SessionMapping.Action.Get()),
				*GetNameSafe(DefaultMapping.Action.Get())));
		}
	}

	return true;
}

/**
 * S6 — THE CAMERA ONLY LOOKS WHILE THE RIGHT MOUSE BUTTON IS HELD, AND HOVER STILL DOES NOT NEED IT.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * `IMC_MouseLook`'s one `Mouse2D` → `IA_MouseLook` mapping carries exactly one
 * `UInputTriggerChordAction`, and that chord names the `IA_LookModifier` asset ITSELF — while every
 * `IA_HoverPiece` mapping in `IMC_Default` carries no chord at all.
 *
 * =====================================================================================
 * WHY THE CHORD IS THE WHOLE OF S6
 * =====================================================================================
 *
 * SESSION_UI_DESIGN §d states what was broken: `IMC_MouseLook` bound the raw `Mouse2D` axis with no
 * held button, so the camera followed the mouse all the time and there was no pointer. The only way
 * a cursor ever appeared was `SetPieceMenuControls(true)` removing the entire context — a coherent
 * design for a game whose only UI is a transient menu, and incompatible with a toolbar that is
 * always on screen, since a permanent strip you can only click by first opening a piece menu is not
 * a toolbar. That function no longer exists: S6 deleted it.
 *
 * The chord replaces the removal. Once look is gated on a held button there is nothing to take away
 * when a panel opens, so the apply/restore pair that could fall out of step simply stops existing.
 * That is why the pins in Tests/PieceMenuPresenterTest.cpp and Tests/PieceMenuChoiceTest.cpp are
 * inverted in the same slice rather than deleted: the hazard they guarded is real and is now closed
 * by a different mechanism, and their new job is to say that nothing removes the context any more.
 *
 * =====================================================================================
 * OBJECT IDENTITY, NOT A NAME
 * =====================================================================================
 *
 * The chord is asserted to be the very `UInputAction` the path loads, by pointer. A
 * chord pointing at a different asset that happens to be called `IA_LookModifier` — a
 * duplicate left in another folder, exactly what a copy-paste authoring step produces
 * — would satisfy any name-shaped assertion and would leave the camera dead, because
 * the action the mapping context actuates is not the action the chord is watching. The
 * same identity rule `BuildPieceMaterial` keeps for material profiles, applied to content.
 *
 * AND THE HOVER MAPPING IS ASSERTED UNCHANGED, the half that would be missed. Hovering
 * is how a player finds the brick they are about to delete, and it reads the same
 * `Mouse2D` axis through a second action precisely so it survives whatever happens to
 * free-look (`RequiredContent.h` records that reasoning). A chord copied onto
 * IA_HoverPiece's mapping — the obvious slip, since both are mouse-move mappings and
 * the editor's copy of a trigger list is one click — would mean nothing highlights
 * unless the player is already holding RMB to spin the camera, the one moment they
 * are not looking for a brick. CURRENT_STATE (i) logs folding the two actions
 * together as a later content change for this exact reason; until then they must
 * differ here.
 *
 * Needs a ticking world: no. Three assets loaded by path.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionLookNeedsTheModifierTest,
	"DestructionGame.Content.SessionInput.LookNeedsTheModifierHeld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionLookNeedsTheModifierTest::RunTest(const FString& Parameters)
{
	using namespace SessionAssetsTestSupport;

	const UInputAction* const LookModifier = LoadObject<UInputAction>(
		nullptr, SessionLookModifierActionPath);

	const UInputAction* const LookAction = LoadObject<UInputAction>(
		nullptr, DestructionContent::MouseLookActionPath);

	const UInputAction* const HoverAction = LoadObject<UInputAction>(
		nullptr, DestructionContent::HoverPieceActionPath);

	const UInputMappingContext* const MouseLookContext = LoadObject<UInputMappingContext>(
		nullptr, DestructionContent::MouseLookMappingContextPath);

	const UInputMappingContext* const DefaultContext = LoadObject<UInputMappingContext>(
		nullptr, DestructionContent::DefaultMappingContextPath);

	TestNotNull(
		*FString::Printf(
			TEXT("holding a button to look needs an input action at '%s'; it does not resolve"),
			SessionLookModifierActionPath),
		LookModifier);

	TestNotNull(
		*FString::Printf(TEXT("fixture: '%s' should resolve"),
			DestructionContent::MouseLookActionPath),
		LookAction);

	TestNotNull(
		*FString::Printf(TEXT("fixture: '%s' should resolve"),
			DestructionContent::HoverPieceActionPath),
		HoverAction);

	TestNotNull(
		*FString::Printf(TEXT("fixture: '%s' should resolve"),
			DestructionContent::MouseLookMappingContextPath),
		MouseLookContext);

	TestNotNull(
		*FString::Printf(TEXT("fixture: '%s' should resolve"),
			DestructionContent::DefaultMappingContextPath),
		DefaultContext);

	if (LookModifier == nullptr || LookAction == nullptr || HoverAction == nullptr
		|| MouseLookContext == nullptr || DefaultContext == nullptr)
	{
		return true;
	}

	AddInfo(FString::Printf(
		TEXT("IMC_MouseLook maps: %s"), *DescribeSessionContext(MouseLookContext)));

	const TArray<FEnhancedActionKeyMapping>& LookMappings = MouseLookContext->GetMappings();

	TestEqual(
		FString::Printf(
			TEXT("fixture: IMC_MouseLook carries exactly one mapping — the raw Mouse2D axis — and this "
				 "whole claim is about that one; it carries %d [%s]"),
			LookMappings.Num(), *DescribeSessionContext(MouseLookContext)),
		LookMappings.Num(), 1);

	if (LookMappings.Num() != 1)
	{
		return true;
	}

	const FEnhancedActionKeyMapping& LookMapping = LookMappings[0];

	AddInfo(FString::Printf(
		TEXT("that mapping's triggers: %s"), *DescribeSessionTriggers(LookMapping)));

	TestTrue(
		*FString::Printf(
			TEXT("fixture: and it must still be the free-look action it has always been; it names %s"),
			*GetNameSafe(LookMapping.Action.Get())),
		LookMapping.Action.Get() == LookAction);

	const UInputAction* Chorded = nullptr;

	const int32 Chords = CountSessionChords(LookMapping, Chorded);

	TestEqual(
		FString::Printf(
			TEXT("THE CAMERA MUST ONLY TURN WHILE A BUTTON IS HELD: the Mouse2D mapping needs exactly "
				 "one Chorded Action trigger. Without it the camera follows the mouse all the time and "
				 "a permanent cursor is unusable; with two, the second is a condition nothing on screen "
				 "explains. It has %d [%s]"),
			Chords, *DescribeSessionTriggers(LookMapping)),
		Chords, 1);

	if (Chords == 1)
	{
		TestTrue(
			*FString::Printf(
				TEXT("and the chord must name the VERY IA_LookModifier asset the session maps to the "
					 "right mouse button — a chord on a same-named duplicate satisfies every "
					 "name-shaped check and leaves the camera dead. It names %s"),
				*GetNameSafe(Chorded)),
			Chorded == LookModifier);
	}

	/*
	 * AND HOVER IS UNCHANGED. It reads the same axis through a second action so that it survives
	 * whatever happens to free-look; a chord copied onto it would mean nothing highlights unless the
	 * player is already holding RMB to spin the camera.
	 */
	int32 HoverMappings = 0;

	for (const FEnhancedActionKeyMapping& Mapping : DefaultContext->GetMappings())
	{
		if (Mapping.Action.Get() != HoverAction)
		{
			continue;
		}

		++HoverMappings;

		const UInputAction* HoverChorded = nullptr;

		TestEqual(
			FString::Printf(
				TEXT("HOVER MUST WORK WITHOUT RMB. IA_HoverPiece's mapping in IMC_Default must carry no "
					 "Chorded Action at all, or the brick under the cursor stops being called out at "
					 "exactly the moment the player is pointing at it rather than spinning the camera. "
					 "It carries %d [%s]"),
				CountSessionChords(Mapping, HoverChorded), *DescribeSessionTriggers(Mapping)),
			CountSessionChords(Mapping, HoverChorded), 0);
	}

	TestTrue(
		FString::Printf(
			TEXT("fixture: IMC_Default must still map the hover action at all for the claim above to "
				 "mean anything; it maps it %d time(s)"),
			HoverMappings),
		HoverMappings >= 1);

	return true;
}

/**
 * S6 — the session's context is actually applied, and each of its eight shortcuts
 * reaches a handler exactly once, on the press.
 *
 * BEHAVIOUR: a controller with a real local player applies `IMC_Session` alongside
 * `IMC_Default` and `IMC_MouseLook`, and `SetupInputComponent` binds each of the eight
 * session actions to exactly one handler on `Started`.
 *
 * THE CONTEXT AND THE BINDINGS ARE ONE CLAIM, because either alone is a dead key: an
 * action mapped in a context that is never applied fires nothing; an applied context
 * whose action nothing binds fires nothing. The two failures are indistinguishable
 * from the player's chair — a key that does nothing — and they are fixed in different
 * files, so a test that saw only one would send the reader to the wrong half.
 * `Content.SessionInput.*` next door owns the shape of the assets; this owns the wire
 * between them and the controller.
 *
 * Started, not Triggered, and exactly one binding. These are one-shot presses of
 * digital keys: with no explicit trigger asset, Triggered fires on every frame the
 * key is held, so holding `]` would walk the build plane up the wall at sixty courses
 * a second and holding `Enter` would re-settle the structure on every frame —
 * `SolveAndPush` releases pieces and its own header is emphatic that releasing is
 * irreversible. Completed is the release, which would run the command on let-go.
 * Started is the press. And exactly one, not at least one: two bindings on `Tab`
 * toggle the mode twice per press, which appears to do nothing at all; two on `Enter`
 * settle twice. A count is the only thing that sees that.
 *
 * The actions are loaded by path rather than read off the controller, so a controller
 * that bound some other asset fails here rather than agreeing with itself.
 *
 * Needs a ticking world: it needs a world — the engine only runs SetupInputComponent
 * for a controller with a real ULocalPlayer behind it — but it never ticks one,
 * spawns no wall and touches no physics.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionShortcutsReachHandlersTest,
	"DestructionGame.World.Input.SessionShortcutsReachHandlers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionShortcutsReachHandlersTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace SessionAssetsTestSupport;

	const UInputMappingContext* const SessionContext = LoadObject<UInputMappingContext>(
		nullptr, SessionMappingContextPath);

	const UInputMappingContext* const DefaultContext = LoadObject<UInputMappingContext>(
		nullptr, DestructionContent::DefaultMappingContextPath);

	const UInputMappingContext* const MouseLookContext = LoadObject<UInputMappingContext>(
		nullptr, DestructionContent::MouseLookMappingContextPath);

	TestNotNull(
		*FString::Printf(TEXT("the session's keyboard needs '%s'; it does not resolve"),
			SessionMappingContextPath),
		SessionContext);

	TestNotNull(
		*FString::Printf(TEXT("fixture: '%s' should resolve"),
			DestructionContent::DefaultMappingContextPath),
		DefaultContext);

	TestNotNull(
		*FString::Printf(TEXT("fixture: '%s' should resolve"),
			DestructionContent::MouseLookMappingContextPath),
		MouseLookContext);

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

	UEnhancedInputLocalPlayerSubsystem* const Subsystem = InputSubsystemOf(Controller);

	TestNotNull(
		TEXT("fixture: a controller with a local player should have an Enhanced Input subsystem"),
		Subsystem);

	/*
	 * FIXTURE PRECONDITION: THE ENGINE REALLY DID RUN SetupInputComponent, and it made an ENHANCED
	 * one. DefaultInput.ini names UEnhancedInputComponent as the default input component class; if
	 * that ever changes, this is where it says so rather than reporting absent bindings.
	 */
	UEnhancedInputComponent* const Input = Cast<UEnhancedInputComponent>(Controller->InputComponent);

	TestNotNull(
		*FString::Printf(
			TEXT("fixture: the controller's input component should be a UEnhancedInputComponent, it is %s"),
			*GetNameSafe(Controller->InputComponent)),
		Input);

	if (Subsystem == nullptr || Input == nullptr)
	{
		TestWorld.End();
		return true;
	}

	AddInfo(FString::Printf(
		TEXT("the controller bound: %s"), *DescribeSessionActionBindings(Input)));

	/* --- ONE: all three contexts are up at once ------------------------------------------- */

	if (DefaultContext != nullptr)
	{
		TestTrue(
			TEXT("fixture: IMC_Default must be applied, or this sweep is about a controller that "
				 "applied nothing"),
			Subsystem->HasMappingContext(DefaultContext));
	}

	if (MouseLookContext != nullptr)
	{
		TestTrue(
			TEXT("fixture: and IMC_MouseLook with it — the chorded free-look context is applied for "
				 "the whole session now, not taken away and put back"),
			Subsystem->HasMappingContext(MouseLookContext));
	}

	if (SessionContext != nullptr)
	{
		TestTrue(
			TEXT("IMC_Session MUST BE APPLIED ALONGSIDE THEM. Nine mappings in a context nothing "
				 "applies is nine keys that do nothing, and it looks exactly like eight missing "
				 "BindAction calls from the player's chair"),
			Subsystem->HasMappingContext(SessionContext));
	}

	/* --- TWO: one handler per shortcut, on the press --------------------------------------- */

	for (const FSessionShortcutRow& Row : SessionBoundShortcutRows())
	{
		const UInputAction* const Action = LoadObject<UInputAction>(nullptr, Row.ActionPath);

		TestNotNull(
			*FString::Printf(TEXT("'%s' (%s) must resolve"), Row.ActionPath, Row.What),
			Action);

		if (Action == nullptr)
		{
			continue;
		}

		int32 Bindings = 0;
		TArray<ETriggerEvent> Triggers;

		for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding
			: Input->GetActionEventBindings())
		{
			if (Binding.IsValid() && Binding->GetAction() == Action)
			{
				++Bindings;
				Triggers.Add(Binding->GetTriggerEvent());
			}
		}

		TestEqual(
			FString::Printf(
				TEXT("SetupInputComponent must bind '%s' (%s) to exactly one handler; it bound %d [%s]"),
				Row.ActionPath, Row.What, Bindings, *DescribeSessionActionBindings(Input)),
			Bindings, 1);

		if (Bindings != 1)
		{
			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("and on Started, because '%s' is a one-shot press: Triggered fires on every frame "
					 "the key is HELD, which walks the course stepper up a wall and re-settles the "
					 "structure sixty times a second. It fires on %s"),
				Row.What, *UE::Input::LexToString(Triggers[0])),
			Triggers[0] == ETriggerEvent::Started);
	}

	/* --- THREE: and the one action that must reach NOTHING --------------------------------- */

	{
		/*
		 * IA_LookModifier is the row SessionBoundShortcutRows removes, and nothing
		 * said so until now. The sweep above is over eight rows because the ninth has
		 * no handler to reach — the modifier exists only to be the action
		 * IMC_MouseLook's chord watches. But a table that drops a row cannot fail on
		 * it: a stray `BindAction(SessionLookModifierAction, ...)` added by somebody
		 * copying the eight would pass every assertion in this file while turning a
		 * held right button into a toolbar command. The count is therefore asserted
		 * at zero, the only way the removal above means anything.
		 */
		const UInputAction* const LookModifier = LoadObject<UInputAction>(
			nullptr, SessionLookModifierActionPath);

		TestNotNull(
			*FString::Printf(TEXT("fixture: '%s' should resolve"), SessionLookModifierActionPath),
			LookModifier);

		if (LookModifier != nullptr)
		{
			int32 Bindings = 0;

			for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding
				: Input->GetActionEventBindings())
			{
				Bindings += Binding.IsValid() && Binding->GetAction() == LookModifier ? 1 : 0;
			}

			TestEqual(
				FString::Printf(
					TEXT("IA_LookModifier MUST REACH NO HANDLER AT ALL: it is the thing the free-look "
						 "chord watches, not a command, so a C++ binding on it would make holding the "
						 "right mouse button DO something on top of turning the camera. It is bound %d "
						 "time(s) [%s]"),
					Bindings, *DescribeSessionActionBindings(Input)),
				Bindings, 0);
		}
	}

	TestWorld.End();

	return true;
}

/**
 * S6 — THE LOOK MODIFIER'S MAPPING IS EVALUATED BEFORE THE MAPPING THAT CHORDS OFF IT, OR THE CAMERA
 * NEVER TURNS AT ALL.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * In the rebuilt per-player mapping list the `RightMouseButton` → `IA_LookModifier` mapping sits at a
 * STRICTLY LOWER index than the `Mouse2D` → `IA_MouseLook` mapping whose chord names it.
 *
 * =====================================================================================
 * WHY AN INDEX IS THE MECHANISM, AND NOT AN IMPLEMENTATION DETAIL
 * =====================================================================================
 *
 * `UInputTriggerChordAction::UpdateState` answers by reading the CHORD ACTION's `TriggerStateTracker`
 * off the player input, and `UEnhancedPlayerInput::EvaluateInputImpl` resets every mapping's trigger
 * state at the END of the frame rather than at the start — the engine's own comment there says why:
 * "Delay MappingTriggerState reset until here to allow dependent triggers (e.g. chords) access to
 * this tick's values". So a chord only ever sees its modifier if the modifier's mapping was evaluated
 * EARLIER IN THE SAME FRAME. Evaluated later, the chord reads a state that was cleared at the end of
 * the previous frame, answers "not held", and `IA_MouseLook` never triggers however hard the player
 * holds the button.
 *
 * =====================================================================================
 * AND EQUAL PRIORITIES DO NOT ARRANGE THAT — WHICH IS WHY THIS LOOKS HARMLESS
 * =====================================================================================
 *
 * `IEnhancedInputSubsystemInterface::ReorderMappings` puts chording mappings before chorded ones
 * WITHIN ONE CONTEXT. The modifier is in `IMC_Session` and the chorded axis is in `IMC_MouseLook`, so
 * that reorder never sees the pair. Across contexts `RebuildControlMappings` orders by
 * `OrderedInputContexts.ValueSort(priority descending)`, which says nothing whatsoever about two
 * contexts of EQUAL priority — and until this test went red, `SetupInputComponent` added all three
 * of ours at `PieceMenuMappingContextPriority`. Nothing in the assets or the design decided which
 * of the two came out first; it was whatever the sort happened to do. The fix this test drove is
 * `SessionMappingContextPriority`, one step above the other two, so the session's block sorts
 * ahead of both deterministically.
 *
 * MEASURED BEFORE THE FIX (review probe, 2026-09-15): the list ordered `[0] IA_MouseLook<-Mouse2D
 * {Chord}` BEFORE `[1] IA_LookModifier<-RightMouseButton`, and a probe holding the right button
 * while moving the mouse saw the modifier's VALUE held on 14 frames of 14 yet IA_MouseLook
 * Triggered on 0 of 14 and 0° of yaw — the camera did not turn. With `IMC_Session` applied one
 * priority ABOVE the other two, the same probe gives IA_MouseLook Triggered 14/14 while held
 * (+24.5° of yaw) and 0/14 with the button up. (Oracle note: a "did it trigger" reading must be a
 * Triggered delegate or `FInputActionInstance::GetTriggerEvent()`, never `GetActionValue` — the
 * value is only reset on fresh key interaction and reads stale after release.)
 *
 * =====================================================================================
 * WHY NOT ASSERT THE YAW
 * =====================================================================================
 *
 * The yaw is the outcome and would be the better assertion if it were reachable here: producing it
 * needs injected key states across two ticked frames with a possessed pawn and a camera manager,
 * which is a functional test rather than this fixture. The ORDER is the mechanism the yaw is a
 * consequence of, it is an exact integer comparison rather than a threshold on an angle, and it is
 * readable from the very fixture the binding sweep above already stands up.
 *
 * BOTH MAPPINGS ARE ASSERTED PRESENT FIRST, because an empty or truncated view — a rebuild that never
 * landed, a context that stopped being applied — makes an ordering claim vacuously true, and that is
 * the state this should be loudest about. The whole ordered list is logged either way, so a failure
 * carries the evidence rather than only the verdict.
 *
 * NEEDS A TICKING WORLD: it needs a WORLD and a real `ULocalPlayer` (the engine only runs
 * `SetupInputComponent` for one), but it never ticks one — the rebuild is forced through
 * `FModifyContextOptions::bForceImmediately`, which runs the same `RebuildControlMappings` the next
 * tick would have run.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionLookModifierEvaluatesFirstTest,
	"DestructionGame.World.Input.LookModifierEvaluatesBeforeTheChord",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionLookModifierEvaluatesFirstTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace SessionAssetsTestSupport;

	const UInputAction* const LookModifier = LoadObject<UInputAction>(
		nullptr, SessionLookModifierActionPath);

	const UInputAction* const LookAction = LoadObject<UInputAction>(
		nullptr, DestructionContent::MouseLookActionPath);

	TestNotNull(
		*FString::Printf(TEXT("fixture: '%s' should resolve"), SessionLookModifierActionPath),
		LookModifier);

	TestNotNull(
		*FString::Printf(TEXT("fixture: '%s' should resolve"), DestructionContent::MouseLookActionPath),
		LookAction);

	if (LookModifier == nullptr || LookAction == nullptr)
	{
		return true;
	}

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

	UEnhancedInputLocalPlayerSubsystem* const Subsystem = InputSubsystemOf(Controller);

	TestNotNull(
		TEXT("fixture: a controller with a local player should have an Enhanced Input subsystem"),
		Subsystem);

	if (Subsystem == nullptr)
	{
		TestWorld.End();
		return true;
	}

	/*
	 * THE LIST IS BUILT ON DEMAND, SO IT IS ASKED FOR RATHER THAN WAITED FOR. AddMappingContext only
	 * marks a rebuild pending; the rebuild itself happens on the subsystem's next tick, and this
	 * fixture never ticks. bForceImmediately runs exactly the RebuildControlMappings that tick would
	 * have run, in this call, which is what makes the claim readable without a frame.
	 */
	FModifyContextOptions Options;
	Options.bForceImmediately = true;

	Subsystem->RequestRebuildControlMappings(Options, EInputMappingRebuildType::Rebuild);

	UEnhancedPlayerInput* const PlayerInput = Subsystem->GetPlayerInput();

	TestNotNull(
		TEXT("fixture: the local player's subsystem should have a UEnhancedPlayerInput behind it"),
		PlayerInput);

	if (PlayerInput == nullptr)
	{
		TestWorld.End();
		return true;
	}

	const TConstArrayView<const FEnhancedActionKeyMapping> Mappings =
		PlayerInput->GetEnhancedActionMappingsView();

	AddInfo(FString::Printf(
		TEXT("the rebuilt mapping list, in the order a frame evaluates it: %s"),
		*DescribeSessionMappingOrder(Mappings)));

	/*
	 * AND THE PRIORITIES, SINCE EQUAL ONES ARE THE WHOLE REASON THE ORDER IS UNDECIDED. Logged rather
	 * than asserted: WHICH priority each context is applied at is a means, and pinning three numbers
	 * here would forbid every fix but one. What must hold is the ORDER below.
	 */
	{
		const TCHAR* const ContextPaths[] = {
			DestructionContent::DefaultMappingContextPath,
			DestructionContent::MouseLookMappingContextPath,
			SessionMappingContextPath
		};

		for (const TCHAR* const Path : ContextPaths)
		{
			const UInputMappingContext* const Context =
				LoadObject<UInputMappingContext>(nullptr, Path);

			int32 Priority = -1;

			const bool bApplied =
				Context != nullptr && Subsystem->HasMappingContext(Context, Priority);

			AddInfo(FString::Printf(
				TEXT("%s: %s, priority %d"),
				Path, bApplied ? TEXT("applied") : TEXT("NOT APPLIED"), Priority));
		}
	}

	const int32 ModifierIndex =
		IndexOfSessionMapping(Mappings, LookModifier, EKeys::RightMouseButton);

	const int32 LookIndex = IndexOfSessionMapping(Mappings, LookAction, EKeys::Mouse2D);

	/* --- FAIL CLOSED FIRST: an absent mapping makes any ordering claim free ----------------- */

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the rebuilt list must carry RightMouseButton -> IA_LookModifier at all — an "
				 "ordering claim over a list that does not contain it is vacuously true. The list is "
				 "[%s]"),
			*DescribeSessionMappingOrder(Mappings)),
		ModifierIndex != INDEX_NONE);

	TestTrue(
		*FString::Printf(
			TEXT("fixture: and Mouse2D -> IA_MouseLook, for the same reason. The list is [%s]"),
			*DescribeSessionMappingOrder(Mappings)),
		LookIndex != INDEX_NONE);

	/* --- THE CLAIM: the modifier is evaluated first ----------------------------------------- */

	if (ModifierIndex != INDEX_NONE && LookIndex != INDEX_NONE)
	{
		TestTrue(
			*FString::Printf(
				TEXT("THE MODIFIER'S MAPPING MUST BE EVALUATED BEFORE THE ONE THAT CHORDS OFF IT: "
					 "RightMouseButton -> IA_LookModifier is at index %d and Mouse2D -> IA_MouseLook at "
					 "index %d. UInputTriggerChordAction::UpdateState reads the CHORD ACTION's "
					 "TriggerStateTracker, and EvaluateInputImpl resets every mapping's trigger state at "
					 "the END of the frame ('Delay MappingTriggerState reset until here to allow "
					 "dependent triggers (e.g. chords) access to this tick's values') — so a chord "
					 "evaluated FIRST reads a state cleared last frame, answers 'not held', and the "
					 "camera never turns however hard the right button is held. ReorderMappings only "
					 "orders chording-before-chorded WITHIN one context, and these two live in "
					 "IMC_Session and IMC_MouseLook; across contexts the order is a ValueSort on "
					 "priority, which decides NOTHING between contexts of EQUAL priority — IMC_Session "
					 "must sit one step ABOVE the other two (SessionMappingContextPriority). The list is [%s]"),
				ModifierIndex, LookIndex, *DescribeSessionMappingOrder(Mappings)),
			ModifierIndex < LookIndex);
	}

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
