---
name: dev-expert
description: Write production code in DestructionGame — but ONLY for behaviour that already has a failing test. Use for implementing any feature, actor, system or bug fix, and for refactoring. Enforces a hard gate: no failing test, no code.
---

# Development Expert (dispatcher)

Spawn the **`dev-expert` subagent** to do this work. Do not do it yourself — the TDD gate only means something if the agent enforcing it verifies the red step independently, rather than being the same context that wrote the test.

Call the Agent tool with `subagent_type: "dev-expert"` and a prompt containing:

- What to implement
- **The exact red output**, quoted — test path, result, and the failing assertion text. The agent will re-run it regardless, but this tells it what it should be seeing.
- Where the declaration and any relevant constants live
- Any constraint that must hold, such as existing expectations that must not move

The agent starts cold and reads `CLAUDE.md`, `DESIGN.md` and `CURRENT_STATE.md` itself. Pass anything that is *only* in this conversation.

Expect it to refuse if the test is not genuinely red — that is the feature working, not a malfunction. Route it back for a test rather than arguing.

Its instructions live in [.claude/agents/dev-expert.md](../../agents/dev-expert.md) — the single source of truth. Edit it there, not here.
