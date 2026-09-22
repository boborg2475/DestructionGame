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
 * Uniquely named, not anonymous: a unity build merges files, so anonymous names can collide.
 * The `using namespace` lives inside each RunTest for the same reason.
 */
namespace SessionAssetsTestSupport
{
	/*
	 * Paths spelled here on purpose, not imported from RequiredContent.h, so a missing asset
	 * fails an assertion rather than the build. Content.RequiredAssetsResolve keeps the two
	 * spellings in step.
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

	/** One shortcut: action asset, key, and description. */
	struct FSessionShortcutRow
	{
		const TCHAR* ActionPath = nullptr;
		FKey Key;
		const TCHAR* What = nullptr;
	};

	/**
	 * The session keyboard as data, transcribed from SESSION_UI_DESIGN §b (reasons for each key
	 * are recorded there). Right mouse is the look modifier (§d), not a command.
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

	/** Every mapping in a context, for the log. */
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

	/** Every trigger on a mapping, including what a chord names, for the log. */
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

	/** Every action-event binding on a component, for the log. */
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
	 * The eight shortcuts that reach a handler: all rows but the look modifier, which exists only
	 * for IMC_MouseLook's chord to watch.
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

	/** The rebuilt per-player mapping list with indices and triggers, in evaluation order. */
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
	 * Index of this action on this key, or INDEX_NONE. Matches both, since Mouse2D also carries
	 * IA_HoverPiece.
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

	/** Floor on IMC_Default's mappings so the collision sweep cannot pass vacuously (it had eight). */
	constexpr int32 SessionDefaultMappingFloor = 6;

	/** IMC_Session's exact mapping count. */
	constexpr int32 SessionMappingCount = 9;
}

/**
 * S6: IMC_Session maps exactly nine Boolean actions, each once, to the key SESSION_UI_DESIGN §b
 * names. Boolean because each is a press; an axis would fire on frames nothing was pressed. The
 * count is exact, since an extra key would do something the toolbar does not show. A hand-authored
 * mapping that is never asserted can vanish on re-save. No world needed; loads assets by path.
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
 * S6: no key IMC_Session maps is already mapped by IMC_Default. bConsumeInput defaults to true,
 * so a shared key would be taken from the pawn silently. Diffs the two assets' key sets rather
 * than listing forbidden keys, so it tracks IMC_Default's contents. Mapping-count floors stop it
 * passing vacuously. No world needed.
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

	// Exact count, not `>= 1`, so a context that lost mappings cannot pass the sweep below.
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
 * S6: IMC_MouseLook's Mouse2D mapping carries exactly one chord trigger, naming the IA_LookModifier
 * asset itself (by pointer, so a same-named duplicate fails), so the camera turns only while RMB is
 * held and a cursor is always usable (SESSION_UI_DESIGN §d). IA_HoverPiece's mapping in IMC_Default
 * must carry no chord, or hover would only work while spinning the camera. No world needed.
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

	// Hover must stay unchorded, or it only works while RMB is held.
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
 * S6: a controller with a local player applies IMC_Session alongside IMC_Default and
 * IMC_MouseLook, and binds each of the eight session actions to exactly one handler on Started.
 * Context and binding are checked together since either missing gives the same dead key. Started,
 * not Triggered, which fires every held frame (holding Enter would re-settle, irreversibly, every
 * frame); exactly one, since two on Tab would toggle twice. Actions load by path so the controller
 * cannot agree with itself. Needs a world (SetupInputComponent needs a ULocalPlayer), never ticked.
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

	// Precondition: DefaultInput.ini makes the input component a UEnhancedInputComponent.
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
		 * The look modifier, dropped from the sweep above, must have zero bindings; otherwise a
		 * stray BindAction on it would pass every other check.
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
 * S6: in the rebuilt mapping list, RightMouseButton -> IA_LookModifier has a lower index than the
 * Mouse2D -> IA_MouseLook mapping that chords off it. A chord reads its modifier's trigger state,
 * which EvaluateInputImpl resets at the end of each frame, so a modifier evaluated later reads
 * "not held" and the camera never turns. ReorderMappings only fixes order within one context;
 * across contexts equal priorities leave it undecided, hence SessionMappingContextPriority one step
 * higher. Before the fix a probe saw the modifier held 14/14 frames and IA_MouseLook triggered 0/14.
 *
 * Asserts order (the mechanism) rather than yaw, which would need a functional test. Both mappings
 * are asserted present first so the ordering claim cannot pass vacuously. Needs a world and
 * ULocalPlayer, never ticked: bForceImmediately runs the rebuild now.
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

	// The rebuild normally waits for the next tick; force it now since this fixture never ticks.
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

	// Priorities are logged, not asserted: only the resulting order is the claim.
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
