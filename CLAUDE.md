# DestructionGame

A realistic physics-destruction game built in Unreal Engine 5.8 on Chaos, written in C++. Players build structures and destroy them, with every piece destructible in a materially-believable way — wood splinters, concrete and stone fracture, glass shatters. The core idea is real-time structural integrity: pieces carry real mass, are held together by connections whose strength depends on the *direction* of the force (compression vs. shear vs. tension), and load redistributes as pieces are removed — so pulling out enough support makes a structure collapse under its own weight rather than crumbling in place. Materials and connection types (mortar, nail, screw, bolt) are data, not code, so behavior is tuned by profile rather than by writing new classes.

Full design decisions and the testing strategy live in [claude_plans/DESIGN.md](claude_plans/DESIGN.md) — read that before implementing anything substantial.

**Want to look at a structure rather than read about one?** [claude_plans/LEVELS.md](claude_plans/LEVELS.md) lists the twenty-nine playable levels — one per fixture the suite measures — and how to join them. No scenario map holds any placed content: the map's *name* selects a catalogue row and the game mode builds everything.

**Adding a level means DUPLICATING a map, never copying the file.** A map's `PrimaryAssetId` is `Map:<the inner UWorld object's name>`, and copying `Lvl_Sandbox.umap` to a new filename leaves that object still called `Lvl_Sandbox` — so every copy claims the same id and **the editor refuses to open any of them**. The `-game` loader never consults the asset manager and loads them happily, which is how twenty-eight broken maps passed a headless check. `Content.ScenarioMapsAreDistinctAssets` is the test that catches it now; `Scripts/New-ScenarioMap.ps1` is the way to make one.

## Units — the easiest way to be wrong by 100×

World scale is Unreal's default **1 uu = 1 cm**. Mass (kg) and density (g/cm³) take published real-world values unconverted, but **1 N = 100 Unreal force units**. Strengths are stored in SI megapascals, so comparing a force against a strength always needs an **area**.

The one conversion boundary is `ForceUnitsPerMPaSqCm` in `Core/ConnectionStrength.h`. Use it; never open-code the factor. A missing or duplicated conversion is out by exactly 100×, which tuned thresholds conceal well. Full table in [DESIGN.md §3](claude_plans/DESIGN.md).

## Comment style — blocks use `/* */`, not stacked `//`

**Two or more consecutive whole-line comments are written as a single `/* ... */` block**, never as a stack of `//` lines:

```cpp
/*
 * Normalize returns false for a zero-length AND for a NaN normal — every
 * comparison against NaN is false, so its length test rejects both.
 */
```

A **single** comment line on its own stays `//`, and so does a **trailing** comment after code (`int X = 1; // note`). A blank line between comment lines is a paragraph break: it makes two blocks, not one.

This **deliberately diverges from Epic's coding standard**, which prefers `//` for everything but doc comments. It is house style, not drift — most explanation in this codebase runs to paragraphs about *why* a threshold is where it is, and a paragraph reads better as prose than as a column of slashes. Don't "fix" it back.

It also **overrides "match the surrounding code"** for comment delimiters specifically — naming, formatting and the comment *density* of the file you are editing still follow the surrounding code, but the delimiters follow this rule.

`Scripts/Convert-CommentBlocks.ps1` enforces it. `-Check` reports without writing (exit 1 if there is work, 2 if it refused a file); no flag converts. It lexes strings and existing block comments rather than pattern-matching lines, and refuses any file it cannot transform safely rather than guessing. Its own tests are `Scripts/Convert-CommentBlocks.Tests.ps1` — run them before trusting it:

```bash
powershell -NoProfile -File Scripts/Convert-CommentBlocks.Tests.ps1
```

Scope is `Source/**/*.h` and `Source/**/*.cpp`. `*.Build.cs` and `*.Target.cs` are C#, and out of scope.

## Current state and the running TODO list

[claude_plans/CURRENT_STATE.md](claude_plans/CURRENT_STATE.md) is a **living** record of where the project stands and everything outstanding. It is not a changelog — it only ever describes the present and the not-yet-done.

Two rules, both mandatory:

- **When you finish something, remove it from CURRENT_STATE.md** as part of that work. Before calling any task done, check the file and delete the entries it resolved. Stale completed items make the list untrustworthy, which makes it useless.
- **When you defer something, add it to CURRENT_STATE.md** before moving on — with enough context that someone can pick it up cold. This includes anything discovered mid-task and consciously left alone.

Read it at the start of a session, and update it at the end of one. It's the first place to look for what to do next.

## A commit is a checkpoint, not a finish line

**Committing or pushing is never the end of a task. If any part of the requested work is still outstanding, keep going after the commit without being asked and without reporting back as though you were done.**

This exists because the failure is so easy to miss from the inside: a commit produces a satisfying, complete-looking summary — files changed, tests green, message written — and that summary reads like a finished job even when three of the five things asked for are untouched. Stopping there hands back work that *looks* delivered, which is worse than obviously stopping early, because it takes the reader's word for it.

So the rule is mechanical rather than a matter of judgement:

- **Do not commit until the tests are done and verified.** A commit is a claim that what it contains works, and a green suite you have *run yourself* is the only thing that backs the claim. Deliberately-red tests from an in-flight slice are the one exception, and a commit carrying one must say so in its message and say why. Never commit around a failure you have not explained.
- **Within that, commit at every verified point rather than batching.** Once a slice is green, commit it — long tasks stay recoverable that way, and this rule must not become an excuse for one enormous commit at the end. The gate is *verified*, not *finished*.
- **After every commit, re-read the task list.** [claude_plans/CURRENT_STATE.md](claude_plans/CURRENT_STATE.md) and the session's own todo list are both authoritative. If either has an open item inside the requested scope, resume immediately.
- **Only stop when the whole ask is done, or when you are genuinely blocked.** Blocked means a decision only the user can make, or a failure you cannot get past — not "this next part is large".
- **When you do stop with work outstanding, say exactly what is left and why**, in the same message. Never let a green test run or a clean commit stand in for that statement.

The same applies to a subagent handing back: `dev-expert` reporting green is one slice finished, not the task finished. Check what remains before reporting up.

## Development process: TDD is mandatory

All development on this project is test-driven. No exceptions, and the rule is not waivable per-task.

1. **Red** — write a test for the behavior and run it. Confirm it fails, and that it fails because the behavior is missing rather than because the test is broken.
2. **Green** — write the minimum production code that makes it pass. No speculative extras.
3. **Refactor** — clean up with the test as the safety net, then re-run.

**No production code gets written for behavior that has no failing test.** A test that was never executed doesn't count as a red step. A test that passes before the change isn't driving anything. The only exemptions are build configuration (`*.Build.cs`, `*.Target.cs`, `.uproject`), content/asset changes, and `.ini` config — anything containing branching logic or arithmetic is not exempt.

Test design specifics — gravity off for unit force isolation, gravity on for integration, assert on mechanism vs. outcome, and why displacement is never a valid break assertion — are in [DESIGN.md §4](claude_plans/DESIGN.md).

### Running the tests

Tests live in `Source/DestructionGame/Tests/`, inside the game module and guarded by `#if WITH_DEV_AUTOMATION_TESTS`. No separate test module — UBT compiles everything under the module directory, so a new test is just a new file.

**Close the Unreal editor first** — it locks `UnrealEditor-DestructionGame.dll` and the build will fail at link.

Build:

```bash
"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" DestructionGameEditor Win64 Development -Project="C:\Users\bobby\Documents\Unreal Projects\DestructionGame\DestructionGame.uproject" -WaitMutex
```

Run all project tests headless:

```bash
"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\bobby\Documents\Unreal Projects\DestructionGame\DestructionGame.uproject" -ExecCmds="Automation RunTests DestructionGame" -TestExit="Automation Test Queue Empty" -unattended -nopause -nosplash -nullrhi -NoSound -log
```

Narrow the run by lengthening the path after `RunTests` (e.g. `DestructionGame.Core.ConnectionLoad`).

**The LP oracle's sweep is opt-in, in two tiers.** Neither name contains `DestructionGame`, so the full-suite command never pays for them:

```
-ExecCmds="Automation RunTests OracleSweepFast"   # 9 tests, ~90 s — iteration
-ExecCmds="Automation RunTests OracleSweepFull"   # 4 tests, ~22 min — verification
-ExecCmds="Automation RunTests OracleSweep"       # both (13), by substring match on the stem
```

Counts as of 2026-08-18. `Run-OracleSweep.ps1` asserts them — a test in a tier but in no parallel bucket runs nowhere, which is how a solver-scale test was silently skipped once.

**`OracleSweepFull` is mandatory before any commit that touches the LP oracle, and before any commit at all if the solver changed.** Those three tests are 94% of the cost because they are the three that watch the solver at scale; a green fast tier verifies none of it. An opt-in tier rots — see TRAPS.

`Scripts/Run-OracleSweep.ps1` runs a tier one test per process (`-Tier All` ≈ 8 min against 22 serial), and `-Verify` runs both ways and diffs every reading to prove the split changed no answer.

**Results do not go to stdout.** The command's own output is only SDK validation noise, and it exits 0 even when tests fail — so never judge a run by its exit code. Read `Saved/Logs/DestructionGame.log` and grep for `LogAutomationController`:

```bash
grep -E "Test Completed|LogAutomationController: Error" "Saved/Logs/DestructionGame.log"
```

Writing the automation macro: the `EAutomationTestFlags` spelling changed across UE 5.x. In 5.8 the context masks are free constants, not enum members — `EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter`. See `Tests/ConnectionLoadTest.cpp` for a working example.

### Functional-test actors live only in never-cooked maps

`AFunctionalTest` and every subclass may be placed **only** in maps under **`Content/Maps/FunctionalTests/`**. A gameplay map — `Content/Maps/Lvl_Sandbox` today — must never contain one.

A functional test is an actor serialized into the `.umap`, and its class lives in Epic's `FunctionalTesting` module, which Epic does not precompile for Shipping. A cooked gameplay map holding that reference carries a **dangling class pointer** into the build. `Config/DefaultGame.ini` keeps the folder out of the cook (`+DirectoriesToNeverCook`); the placement rule is what makes that one line sufficient, so the two only work together.

`FunctionalTesting` is deliberately **not** a dependency in `DestructionGame.Build.cs` — nothing needs it yet. When the first functional test does, add it **gated**, never bare:

```csharp
if (Target.Configuration != UnrealTargetConfiguration.Shipping)
{
    PrivateDependencyModuleNames.Add("FunctionalTesting");
}
```

## Specialized agents

Development goes through three subagents, in this order. Never work the task directly.

| Agent | Owns | Entry condition |
|---|---|---|
| [test-expert](.claude/agents/test-expert.md) | Test design, assertion choice, confirming red | Start of every behavior, feature, or bug fix |
| [dev-expert](.claude/agents/dev-expert.md) | Implementation and refactoring | A failing test exists and has been run |
| [review-expert](.claude/agents/review-expert.md) | Test quality, TDD compliance, design alignment | Tests are green |

They are **subagents, not inline instructions** — each starts with a cold context and reads `CLAUDE.md`, `DESIGN.md` and `CURRENT_STATE.md` itself. That isolation is the point rather than an overhead:

- `dev-expert` enforces the TDD gate by **running the test itself**. A gate you enforce on yourself in the same breath as writing the test isn't much of a gate; one enforced by a context that never saw the test being written is.
- `review-expert` has **no write access at all**. It cannot quietly fix what it should be surfacing.

Invoke them via the Agent tool with `subagent_type`, or through the matching `/test-expert`, `/dev-expert`, `/review-expert` skills, which are thin dispatchers onto the same agents. **The agent definitions in `.claude/agents/` are the single source of truth** — edit behaviour there, never in the skill wrappers.

Pass a subagent anything that exists only in the conversation: the exact red output, values you have already worked out, constraints that must hold. Don't paste the project docs; it reads those.

Expect `dev-expert` to refuse when a test isn't genuinely red, and expect `review-expert` to send work back. Both are the system working. Act on blocking findings, log deferred ones in `CURRENT_STATE.md`, and don't call anything done until review has passed.
