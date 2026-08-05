# Code Tour — reading this project from scratch

**Who this is for:** someone who owns this project, programs in other languages but not C++, is new
to Unreal, and wants to review every line themselves rather than take anyone's word for it.

**It is a reading order, not a tutorial.** It assumes you know how to program. It does not explain
loops, types or dot products. It explains **the things C++ does differently from everything else**,
and it explains Unreal, and it puts both in the order this codebase needs them.

---

## Be realistic about the size

| | Lines |
|---|---|
| Production code | **~6,600** |
| Tests | **~27,400** |
| Total | ~34,000 |

Two things follow, both good news.

**You do not read the tests linearly.** They are 4× the production code. You read a test when you
want to know *what a thing is supposed to do* — in this project the tests are the specification, and
they are usually more explicit about intent than the code is.

**The production code is front-loaded.** Four files (`ConnectionLoad`, `ConnectionStrength`,
`Connection`, `Structure`) are the actual physics. Everything else connects them to Unreal. If you
only ever understood those four, you would understand what this game *is*.

**Honest estimate:** 30–45 hours to do properly. Sessions 1–6 are the ones that matter and are maybe
half of that.

---

## First: the five things C++ does that your other languages don't

Read this once now. Everything else will make more sense.

**1. Value semantics by default. This is the big one.**
In most languages, a variable holding an object holds a *reference*. In C++ it holds *the object*, and
assignment **copies**. This is the source of the single most important bug class in this codebase:

```cpp
for (FConnection C : Connections)   // C is a COPY. Changes to it are thrown away.
for (FConnection& C : Connections)  // C refers to the real one.
```

That missing `&` compiles cleanly, runs, and silently does nothing. It happened here, in the break
sweep, and the result was a structure that could never break. When you review, **check every loop and
every parameter for whether it copies.**

**2. Three ways to refer to something, and the choice is meaningful.**
- `FConnection C` — a copy.
- `FConnection& C` — a reference: an alias for the original. Cannot be null, cannot be reseated.
- `FConnection* C` — a pointer: can be null, can be reassigned, must be checked.

`const FConnection&` is the default way to pass anything bigger than a number: no copy, and a promise
not to modify it.

**3. `const` is part of the type, and the compiler enforces it.**
A method marked `const` cannot modify the object. This is not documentation — it fails the build. The
project uses it as a design tool: `GetStructure()` is const-only *specifically* so that a caller
cannot reach in and mutate the graph behind the binding's back, and there is a `static_assert` that
**breaks the build** if anyone ever adds a non-const version.

**4. Undefined behaviour exists, and it is not an exception.**
Reading past an array, or using a reference to something that has been destroyed, does not throw. It
does *whatever*, possibly working fine today and corrupting memory tomorrow. There is a documented
instance in this codebase: `ChoosePieceMenuRow` copies the chosen row's fields into locals **before**
dismissing the menu, because dismissing resets the array the row lives in — and holding a reference
into it would be reading destroyed memory. **No test can assert undefined behaviour**, which is why
that one is held by a comment.

**5. There are two memory models in this repository at once.**
- **`Core/` is plain C++.** No garbage collector. Objects live in arrays, die when the array dies.
- **`World/` and the actors are Unreal `UObject`s, which *are* garbage collected** by Unreal's own
  collector, bolted on top of C++ via macros. Unreal can delete one while you still hold a pointer.

Knowing which world a file lives in tells you what can go wrong in it. `Core/` cannot have
use-after-free from the GC; the actor layer can.

**Also worth knowing, briefly:**
- **Header (`.h`) / source (`.cpp`) split.** No import system — `#include` is *literal text
  inclusion*. Headers declare, sources define.
- **Macros do real work here.** `UCLASS()`, `UPROPERTY()`, `GENERATED_BODY()` are read by Unreal
  Header Tool, which **generates C++ before compilation**. That is why there are `.generated.h`
  includes and why the build sometimes fails in files you did not write.
- **Unreal has its own standard library.** `TArray` not `std::vector`, `TMap` not `std::map`,
  `FString` not `std::string`. You will see almost no `std::`.
- **Type prefixes are mandatory convention:** `F` plain struct, `U` UObject class, `A` Actor,
  `E` enum, `T` template, `S` Slate widget, `b` boolean variable.

---

## The loop you will use for everything

```
read a file  →  read its test  →  change the code deliberately  →  run the tests  →  see what breaks
```

**The fourth step is the review.** If you change a line and nothing fails, either the line does not
matter or nothing is checking it — **both are findings**. You do not need to understand a file before
you break it; breaking it is how you understand it.

---

## Session 0 — Get the loop working before reading anything

**Close the Unreal editor first** — it locks the compiled DLL and the build fails at link.

```bash
"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" DestructionGameEditor Win64 Development -Project="C:\Users\bobby\Documents\Unreal Projects\DestructionGame\DestructionGame.uproject" -WaitMutex
```

```bash
"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\bobby\Documents\Unreal Projects\DestructionGame\DestructionGame.uproject" -ExecCmds="Automation RunTests DestructionGame" -TestExit="Automation Test Queue Empty" -unattended -nopause -nosplash -nullrhi -NoSound -log
```

**Results are not in stdout, and the command exits 0 even when tests fail.** Read the log:

```bash
grep -E "Test Completed|LogAutomationController: Error" "Saved/Logs/DestructionGame.log"
```

Expect **84 tests, all Success**. Learn to read that log while nothing is wrong.

---

## Session 1 — The build system

**Read:** [DestructionGame.Build.cs](../Source/DestructionGame/DestructionGame.Build.cs) (40),
[DestructionGame.cpp](../Source/DestructionGame/DestructionGame.cpp) (7),
[DestructionGame.Target.cs](../Source/DestructionGame.Target.cs) (15). **~70 lines.**

**Unreal:** the build tool is a **C# program**, so build config is C#, not C++ and not a makefile. A
**module** compiles to a DLL the editor loads; this project has one. `.Build.cs` lists the engine
modules you may touch — read the list and the comments, it tells you the project's surface area.

**Question:** which dependencies exist only because the menu is hand-written C++ UI?

---

## Session 2 — The smallest complete idea

**Read:** [Core/ConnectionLoad.h](../Source/DestructionGame/Core/ConnectionLoad.h) (62) +
[.cpp](../Source/DestructionGame/Core/ConnectionLoad.cpp) (45). **107 lines.**
**Test:** [ConnectionLoadTest.cpp](../Source/DestructionGame/Tests/ConnectionLoadTest.cpp) (218).

Takes a force and a joint's orientation, and splits it into compression, tension and shear.

**C++ here:** free functions (not methods) — which is exactly why this is testable with zero setup.
`const FVector&` parameters throughout. Everything is `double`, deliberately, not `float`.

**Unreal here:** `FVector` is the 3D vector type.

**The concept that matters — units.** 1 unreal unit = 1 cm, mass is in kg, **but 1 newton = 100
unreal force units**. Get it wrong and every answer is off by exactly 100×, which still looks
plausible. This is called out in CLAUDE.md as the easiest way to be wrong in this project.

**Question:** why classify against the joint's own normal rather than against world "up"? The answer
is the whole reason a tilted joint behaves differently — DESIGN.md §2.

---

## Session 3 — Force to "has it broken?"

**Read:** [Core/ConnectionStrength.h](../Source/DestructionGame/Core/ConnectionStrength.h) (121) +
[.cpp](../Source/DestructionGame/Core/ConnectionStrength.cpp) (103). **224 lines.**
**Test:** [ConnectionStrengthTest.cpp](../Source/DestructionGame/Tests/ConnectionStrengthTest.cpp) (628).

Returns a **utilisation ratio**: 0 unloaded, 1 at the limit, >1 the joint gives.

**C++ here:** `constexpr` (compile-time constant), `TNumericLimits<double>::Max()` used as "no cap".

**Concepts that matter:**
- **Stress, not force.** Strengths are megapascals — force *per area* — so an area is always needed.
- **`ForceUnitsPerMPaSqCm` is the one conversion boundary in the project.** If you ever see `10000`
  open-coded anywhere else, that is a bug.
- **Mohr-Coulomb shear**: a joint resists sliding better while it is being squeezed. That is why a
  wall weakens as you remove the weight above it — the heart of the game.

**Question:** what does this return when the area is zero, and why is that the *safe* direction?
(Search "fail closed": when input is wrong, return the answer that fails **visibly** rather than one
that looks healthy.)

---

## Session 4 — The joint

**Read:** [Core/Connection.h](../Source/DestructionGame/Core/Connection.h) (88) +
[.cpp](../Source/DestructionGame/Core/Connection.cpp) (97). **185 lines.**
**Test:** [ConnectionTest.cpp](../Source/DestructionGame/Tests/ConnectionTest.cpp) (897).

One joint. Knows its own orientation, area and material, so callers hand it only a force.

**C++ here — `const` methods as a design boundary.** `UtilisationUnder(...) const` asks a question.
`ApplyForce(...)` asks the same question *and permanently records the answer*. The `const` is what
makes that difference visible in the type rather than in a comment.

**Look for the latch.** Once a joint gives it stays given, and reports zero load forever after. Read
why: a joint that healed when load dropped would let a wall collapse, redistribute, and stand back up
mid-fall.

**Question:** `ApplyForce` is *defined as* `UtilisationUnder` plus the latch rather than having its
own copy of the arithmetic. Why does one copy matter? (Two copies agree to nine decimal places
forever — and still differ in the last bit.)

---

## Session 5 — Data, not code

**Read:** [Profiles/ConnectionProfiles.h](../Source/DestructionGame/Core/Profiles/ConnectionProfiles.h) (66) +
[.cpp](../Source/DestructionGame/Core/Profiles/ConnectionProfiles.cpp) (323),
[MaterialProfiles.h](../Source/DestructionGame/Core/Profiles/MaterialProfiles.h) (58) +
[.cpp](../Source/DestructionGame/Core/Profiles/MaterialProfiles.cpp) (128).
**575 lines, but it is a table — read the headers, skim the rows.**
**Test:** [ProfileLibraryTest.cpp](../Source/DestructionGame/Tests/ProfileLibraryTest.cpp) (808).

Mortar, lime mortar, dry stone, nail, screw, bolt; concrete and clay brick. **Every number cites a
real engineering standard.**

**C++ here:** `namespace` scoping, `enum class` (scoped and type-safe, unlike C enums), and static
tables that cost nothing to look up.

**The design property to review:** materials are **data, not code**. Adding a material must mean
adding a row, never a new class or an `if`. Check that this holds.

**Question:** pick a material, follow one number to the standard cited beside it, and decide whether
you believe it. This is precisely the review you said you wanted to do — and it needs no C++ at all.

---

## Session 6 — The graph (the big one, three sittings)

**Read:** [Core/Structure.h](../Source/DestructionGame/Core/Structure.h) (500) +
[.cpp](../Source/DestructionGame/Core/Structure.cpp) (996). **~1,500 lines, the largest thing here.**
**Tests:** [StructureTest.cpp](../Source/DestructionGame/Tests/StructureTest.cpp) (3911),
[StructureRemovalTest.cpp](../Source/DestructionGame/Tests/StructureRemovalTest.cpp) (2220),
[StructureCascadeTest.cpp](../Source/DestructionGame/Tests/StructureCascadeTest.cpp) (1231).

The whole structural simulation. **Three sittings:**

**6a — the data model.** Pieces, connections, integer handles, and **tombstoning**: removing a piece
marks the slot dead and never reuses it. Read why there is no free list — reuse would make a stale
handle point at a *different live piece*, which is undetectable, while a dangling index is at least
range-checkable.

**6b — `SolveLoads`.** Bed joints (bearing) vs head joints (fallback); which pieces reach the ground;
a topological order so a piece is solved after everything resting on it; load split by area; and a
fixpoint loop, because stranding one piece changes who reaches the ground.

**6c — `SolveAndBreak` and `RemovePiece`.** Break every over-capacity joint at once, stamp the pass
number, re-solve so load moves to neighbours, repeat until a pass breaks nothing.

**C++ here — this is where value semantics bite.**
- `for (FConnection& C : ...)` — **the `&` is load-bearing**, per point 1 at the top. This is the file
  where its absence was a real bug.
- **Integer handles rather than pointers.** Pieces are array indices (`int32`), `INDEX_NONE` (−1)
  means none. Handles survive the array reallocating; pointers would not.
- **Parallel arrays.** Several arrays indexed by the same handle. Read which are rebuilt every solve
  (self-healing) and which are never rebuilt (a pass stamp cannot be recomputed) — the difference is
  the point.

**Question:** why is `EPieceSupport::Falling` deliberately the **zero** value of its enum? Search the
comments for "polarity". The answer is genuinely worth the time: *a value that is safe as an answer
can be unsafe as a command.*

---

## Session 7 — Geometry: where the graph comes from

**Read:** [Core/Layout.h](../Source/DestructionGame/Core/Layout.h) (178) +
[.cpp](../Source/DestructionGame/Core/Layout.cpp) (426). **604 lines.**
**Test:** [LayoutTest.cpp](../Source/DestructionGame/Tests/LayoutTest.cpp) (2126).

Turns "a wall 30 wide and 40 tall" into pieces and joints. The only code that knows where anything
physically is.

**Two traps, both already sprung and fixed here:**

1. **The interface normal is the axis of separation, never the direction between two centres.** Using
   centre-to-centre makes every horizontal joint in a running-bond wall misclassify — and nothing
   crashes. The wall just stands there being wrong by ~41×.
2. **Multiplication order changes the result.** `density × a × b × c` and `a × b × c × density` differ
   in the last bit. You know floats are inexact; what is worth seeing is the *test*, which asserts the
   exact decimal **and** asserts the two orders still disagree — so the day they stop disagreeing, the
   file says so rather than silently losing its coverage.

**Also:** NaN is used deliberately as the fail-closed return, because **zero would have been accepted
as a real answer** (a massless piece is legal) while NaN is rejected by a guard that already exists.

---

## Session 8 — First contact with Unreal objects

**Read:** [Core/StructureBinding.h](../Source/DestructionGame/Core/StructureBinding.h) (258) +
[.cpp](../Source/DestructionGame/Core/StructureBinding.cpp) (292). **550 lines.**
**Test:** [StructureBindingTest.cpp](../Source/DestructionGame/Tests/StructureBindingTest.cpp) (1574).

The seam between "piece 47 in the graph" and "that brick you can see". Still a plain struct — but the
first file that admits `UObject`s exist.

**Unreal — the conceptual one:**
- **`UObject`** is Unreal's managed base class, and Unreal runs **its own garbage collector** over
  them. It can destroy one while you hold a pointer.
- **`UPROPERTY()`** on a member is what tells the collector you are holding it. A raw `UObject*`
  member *without* it can be collected out from under you — a real hazard, not a style rule.
- **`TWeakObjectPtr<UObject>`** — a pointer that knows when its object died and reads null instead of
  dangling. Used here deliberately: a brick can be destroyed by any route and the binding must
  survive it.

**Look for:** the const-only `GetStructure()` and the `static_assert` that **fails the build** if a
non-const overload ever appears. A good example of making a mistake unsayable rather than documented.

---

## Session 9 — An actual Actor

**Read:** [World/BrickActor.h](../Source/DestructionGame/World/BrickActor.h) (109) +
[.cpp](../Source/DestructionGame/World/BrickActor.cpp) (135). **244 lines.**
**Tests:** [BrickActorTest.cpp](../Source/DestructionGame/Tests/BrickActorTest.cpp) (538),
[BrickHighlightMaterialTest.cpp](../Source/DestructionGame/Tests/BrickHighlightMaterialTest.cpp) (326).

**Unreal, and this session is dense:**
- **`AActor`** — a thing that can exist in a level. **Components** do the actual work; a
  `UStaticMeshComponent` has the shape. `RootComponent` is what everything hangs off.
- **The constructor runs at editor startup, not at spawn.** Unreal builds one template instance of
  every class — the **Class Default Object** — when the module loads. This is genuinely surprising
  coming from other languages, and it is why assets are resolved in the constructor via
  `ConstructorHelpers::FObjectFinder`.
- **Mobility.** A `Static` component can never move, ever. Bricks are `Movable` from birth because
  switching later would require recreating the physics body.
- **`SetSimulatePhysics(true)`** hands the object to Chaos. Before that it is *kinematic*: physically
  present, but not falling.

**Look for:** `Release()` returns early if already simulating. Calling it twice recreates the body,
and a recreated body starts **from rest** — so a brick a quarter-second into its fall would freeze in
mid-air with nothing reporting an error.

**Also:** the highlight uses `SetOverlayMaterial`, which draws a second material over the brick. This
is where the Nanite bug lived — see Conventions and gotchas in CURRENT_STATE.md.

---

## Session 10 — The world

**Read:** [World/DestructionStructureSubsystem.h](../Source/DestructionGame/World/DestructionStructureSubsystem.h) (120) +
[.cpp](../Source/DestructionGame/World/DestructionStructureSubsystem.cpp) (402). **522 lines.**
**Tests:** [StructurePushTest.cpp](../Source/DestructionGame/Tests/StructurePushTest.cpp) (613),
[BrickWallScenarioTest.cpp](../Source/DestructionGame/Tests/BrickWallScenarioTest.cpp) (570).

Owns every structure, spawns bricks, pushes solver answers into the world, turns a mouse ray into a
piece.

**C++ here:**
- **`TUniquePtr<T>`** — owns its object and deletes it automatically. C++ has no GC of its own; **RAII**
  (destruction tied to scope) is how it manages lifetime instead.
- **`TArrayView<T>`** — a non-owning window onto an array. Cheap to pass, and **dangles** if the real
  array is resized or destroyed. There is a recorded near-miss about exactly that in CURRENT_STATE.md.

**Unreal here:** `UWorld` is one running level; a **subsystem** is an object whose lifetime is tied to
it, created and destroyed automatically — the modern answer to "where does global state live".

**Look for:** `SolveAndPush` versus `PushSolvedResultsToWorld`. **The split exists because of a real
bug** — deleting a brick re-solved the graph and never told the world, so nothing fell, and 71
passing tests missed it. That story is in CURRENT_STATE.md and is the most instructive failure here.

---

## Session 11 — Actions, menus, selection

**Read:** [Core/PieceActions.h](../Source/DestructionGame/Core/PieceActions.h) (182) +
[.cpp](../Source/DestructionGame/Core/PieceActions.cpp) (301);
[PieceMenu](../Source/DestructionGame/Core/PieceMenu.h) (101+95);
[PieceSelection](../Source/DestructionGame/Core/PieceSelection.h) (56+65). **~800 lines.**
**Test:** [PieceActionsTest.cpp](../Source/DestructionGame/Tests/PieceActionsTest.cpp) (1872).

**C++ here — function pointers.** `bool (*CanRun)(const FStructureBinding&, int32)` is a variable
holding a function. Note what it is **not**: no closure, no captured state, no `std::function`
allocation. That is deliberate — it keeps each table row a compile-time constant. It makes the action
table *data*, so adding "Rotate" is adding a row rather than editing a `switch`.

**Look for:** the menu offers the **intersection** of what every selected brick allows, not the union.
Read why union is the plausible wrong answer.

**And:** the re-solve is the **last statement** and runs **unconditionally**, with deliberately no
public "run without solving". Releasing a brick is irreversible, so a caller who forgot to re-solve
would drop bricks against a stale answer.

---

## Session 12 — Input and the on-screen menu

**Read:** [DestructionGamePlayerController.h](../Source/DestructionGame/DestructionGamePlayerController.h) (259) +
[.cpp](../Source/DestructionGame/DestructionGamePlayerController.cpp) (583);
[GameMode](../Source/DestructionGame/DestructionGameGameMode.cpp) (86);
[RequiredContent.h](../Source/DestructionGame/RequiredContent.h) (86). **~1,000 lines.**

**Unreal:**
- **GameMode / PlayerController / Pawn** — the rules, the player's agent, the body being driven.
- **Enhanced Input.** An *Input Action* is "the player wants to inspect"; an *Input Mapping Context*
  binds it to a key. `ETriggerEvent::Started` fires once on press, `Triggered` every frame. Read why
  the click uses one and the mouse-move uses the other — it is argued in the code.
- **Slate** — Unreal's C++ UI toolkit; the menu is `SButton`s in an `SVerticalBox`.

**Look for:** the rule that the widget contains **no logic at all**. It was written without a test
(a headless world has no viewport, so no test can ever reach it), and that was made acceptable by
shrinking the untestable part until it was trivial. A good model for genuinely untestable code.

---

## Session 13 — How the tests work

**Read:** [Tests/BrickWorldTestSupport.h](../Source/DestructionGame/Tests/BrickWorldTestSupport.h) (549),
then skim [StructureFuzzTest.cpp](../Source/DestructionGame/Tests/StructureFuzzTest.cpp) (2200) and
[StructureIntegrationTest.cpp](../Source/DestructionGame/Tests/StructureIntegrationTest.cpp) (1521).

**Unreal:** `IMPLEMENT_SIMPLE_AUTOMATION_TEST` registers a test (its flags spelling changed across
Unreal versions — copy an existing one). `FTestWorldWrapper` builds a real ticking world in code with
real gravity and no editor.

**Two ideas worth taking:**
- **The fuzzers** generate 20,000 random structures and check them against an **independently written**
  solver — a different algorithm computing the same answer. Disagreement means one is wrong. The seed
  prints, so any failure reproduces exactly.
- **The entry rule:** *if what can be wrong is arithmetic on a graph it belongs in the fast tests; if
  what can be wrong is a call nobody makes, only an integration test reaches it.* That rule exists
  because of the Session 10 bug.

---

## How to verify anything — the technique

This is the part that answers *"make sure it's doing exactly what I need"*.

**1. Read the test, not just the code.** It states the intended behaviour, usually with the reasoning.

**2. Break it on purpose, and predict the outcome first.** Change a line — flip `>` to `>=`, delete a
`&`, remove a guard. **Write down what you expect to fail.** Rebuild, run, compare.

- **Fails as predicted** → you understand it, and it is genuinely covered.
- **Nothing fails** → **you found a real gap.** Either the line does not matter or nothing checks it.
- **Something unexpected fails** → you learned a dependency you did not know about.

Revert with `git checkout -- <file>` — **but only if you have no other uncommitted work in that file**,
since it discards everything. Commit first; it is safer.

**3. Reproduce the recorded mutations.** CURRENT_STATE.md has a table of mutations already run, each
with the exact number of assertions that fired. **This is the best exercise in this document**,
because you have a predicted answer to check against — so you find out immediately whether you
understood the code or only thought you did.

**4. Ask the fuzzers.** For anything in the solver, they already tried 20,000 shapes you would not
think of.

---

## Where to look things up

| Question | Where |
|---|---|
| Why is the design like this? | [DESIGN.md](DESIGN.md) |
| What is done, what is next | [CURRENT_STATE.md](CURRENT_STATE.md) — start at "What to do next" |
| The rules of working here | [CLAUDE.md](../CLAUDE.md) |
| What does this thing do | Its test |
| Why is this line like *that* | The comment above it |

**A note on the comments.** They are long and explain *why*, not *what*. When one says an alternative
was tried and rejected, that is literal — it was written, run, and found wrong. Those are the most
valuable paragraphs in the repository.

---

## Suggested first week

| Day | Do |
|---|---|
| 1 | The five-things section, then Sessions 0 and 1. Get comfortable that you can always get back to green. |
| 2 | Session 2. 107 lines, with its test. Break one line and watch it fail. |
| 3 | Sessions 3 and 4. You now understand how a force becomes a broken joint — the core of the game. |
| 4 | Session 5, skimming tables. Then reproduce one recorded mutation. |
| 5–7 | Session 6, one sitting per part. The hard one; do not rush it. |

After that you will have read ~2,600 lines and will understand the entire physics model. Sessions
7–13 are Unreal plumbing and can be taken in any order as you need them.

**On learning C++ alongside:** you do not need templates, inheritance, move semantics or manual memory
management before Session 6 — this codebase deliberately avoids the deep end. What you *do* need is
value-vs-reference and `const`, which is why they are points 1–3 at the top.
[learncpp.com](https://www.learncpp.com) chapters on references, pointers and `const` are the
targeted reading; skip its beginner half.
