# DestructionGame

A realistic physics-destruction game built in Unreal Engine 5.8 on Chaos, written in C++. Players build structures and destroy them, with every piece destructible in a materially-believable way — wood splinters, concrete and stone fracture, glass shatters. The core idea is real-time structural integrity: pieces carry real mass, are held together by connections whose strength depends on the *direction* of the force (compression vs. shear vs. tension), and load redistributes as pieces are removed — so pulling out enough support makes a structure collapse under its own weight rather than crumbling in place. Materials and connection types (mortar, nail, screw, bolt) are data, not code, so behavior is tuned by profile rather than by writing new classes.

Full design decisions and the testing strategy live in [claude_plans/DESIGN.md](claude_plans/DESIGN.md) — read that before implementing anything substantial.

## Units — the easiest way to be wrong by 100×

World scale is Unreal's default **1 uu = 1 cm**. Mass (kg) and density (g/cm³) take published real-world values unconverted, but **1 N = 100 Unreal force units**. Strengths are stored in SI megapascals, so comparing a force against a strength always needs an **area**.

The one conversion boundary is `ForceUnitsPerMPaSqCm` in `Core/ConnectionStrength.h`. Use it; never open-code the factor. A missing or duplicated conversion is out by exactly 100×, which tuned thresholds conceal well. Full table in [DESIGN.md §3](claude_plans/DESIGN.md).

## Current state and the running TODO list

[claude_plans/CURRENT_STATE.md](claude_plans/CURRENT_STATE.md) is a **living** record of where the project stands and everything outstanding. It is not a changelog — it only ever describes the present and the not-yet-done.

Two rules, both mandatory:

- **When you finish something, remove it from CURRENT_STATE.md** as part of that work. Before calling any task done, check the file and delete the entries it resolved. Stale completed items make the list untrustworthy, which makes it useless.
- **When you defer something, add it to CURRENT_STATE.md** before moving on — with enough context that someone can pick it up cold. This includes anything discovered mid-task and consciously left alone.

Read it at the start of a session, and update it at the end of one. It's the first place to look for what to do next.

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

**Results do not go to stdout.** The command's own output is only SDK validation noise, and it exits 0 even when tests fail — so never judge a run by its exit code. Read `Saved/Logs/DestructionGame.log` and grep for `LogAutomationController`:

```bash
grep -E "Test Completed|LogAutomationController: Error" "Saved/Logs/DestructionGame.log"
```

Writing the automation macro: the `EAutomationTestFlags` spelling changed across UE 5.x. In 5.8 the context masks are free constants, not enum members — `EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter`. See `Tests/ConnectionLoadTest.cpp` for a working example.

## Specialized agents

Development goes through three local skills in `.claude/skills/`, in this order. Invoke them by name (`/test-expert`, `/dev-expert`, `/review-expert`) rather than working the task directly.

| Skill | Owns | Entry condition |
|---|---|---|
| [test-expert](.claude/skills/test-expert/SKILL.md) | Test design, assertion choice, confirming red | Start of every behavior, feature, or bug fix |
| [dev-expert](.claude/skills/dev-expert/SKILL.md) | Implementation and refactoring | A failing test exists and has been run |
| [review-expert](.claude/skills/review-expert/SKILL.md) | Test quality, TDD compliance, design alignment | Tests are green |

`dev-expert` enforces the TDD gate: it verifies a failing test exists by running it, and if one doesn't, it refuses to write code and hands back to `test-expert` first. `review-expert` verifies independently rather than trusting the handoff, and is expected to send work back.

Work isn't done until review has passed.
