# DestructionGame

A realistic physics-destruction game built in **Unreal Engine 5.8** on **Chaos**, written in C++.

Players build structures and destroy them, with every piece destructible in a materially-believable way — wood splinters, concrete and stone fracture, glass shatters. The core idea is **real-time structural integrity**: pieces carry real mass, are held together by connections whose strength depends on the *direction* of the force, and load redistributes as pieces are removed. Pull out enough support and a structure collapses under its own weight rather than crumbling in place.

Materials and connection types (mortar, nail, screw, bolt) are **data, not code**, so behaviour is tuned by profile rather than by writing new classes.

## Why it isn't just Chaos

Chaos connections carry a **single strain threshold** and are agnostic to the *type* of force applied. Real materials are not: stone is enormously strong in compression and brittle in shear and tension, and a brick wall usually fails because the mortar gives, not the brick.

This project puts a directional layer in front of Chaos:

- A force is resolved into **compression, tension and shear relative to the joint's interface plane** — not world axes. The same downward gravity is compression on a horizontal joint and shear on a vertical one.
- Each direction is measured against its **own strength**, in real SI megapascals traceable to published material data.
- Shear capacity follows **Mohr-Coulomb**: it grows with how hard the joint is squeezed, then truncates at the material's own limit. This is why masonry stands, why dry stone works with no mortar at all, and why a wall sheds shear resistance as the load above it is removed.

## Status

Early. The scaffold runs and the directional force model is built and tested; nothing is wired to Chaos geometry yet.

[claude_plans/CURRENT_STATE.md](claude_plans/CURRENT_STATE.md) is the authoritative, living record of where things stand and what's outstanding. [claude_plans/DESIGN.md](claude_plans/DESIGN.md) holds the design decisions and the testing strategy.

| Area | Where |
|---|---|
| Force classification and joint strength | `Source/DestructionGame/Core/` |
| Automation tests | `Source/DestructionGame/Tests/` |
| Sandbox level | `Content/Maps/Lvl_Sandbox` |

## Building and testing

Development is **test-driven without exception** — see [CLAUDE.md](CLAUDE.md) for the process and the full command reference.

**Close the Unreal editor first**; it locks the module DLL and the build will fail at link.

```bash
"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" DestructionGameEditor Win64 Development -Project="C:\Users\bobby\Documents\Unreal Projects\DestructionGame\DestructionGame.uproject" -WaitMutex
```

```bash
"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\bobby\Documents\Unreal Projects\DestructionGame\DestructionGame.uproject" -ExecCmds="Automation RunTests DestructionGame" -TestExit="Automation Test Queue Empty" -unattended -nopause -nosplash -nullrhi -NoSound -log
```

**Test results never reach stdout, and the command exits 0 even when tests fail.** Read the log:

```bash
grep -E "Test Completed|LogAutomationController: Error" "Saved/Logs/DestructionGame.log"
```

## A note on units

Unreal's physics is a **cm / kg / second** system, so mass and density take published real-world values unconverted — but **1 N = 100 Unreal force units**. Strengths are stored in SI and converted at a single named boundary. Get this wrong and everything is out by exactly 100×, which tuned thresholds will happily conceal. Full table in [DESIGN.md §3](claude_plans/DESIGN.md).
