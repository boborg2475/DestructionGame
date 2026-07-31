---
name: test-expert
description: Write the failing test that must exist before any production code in DestructionGame. Use at the START of every new behaviour, feature or bug fix (the TDD red step), when asked to add or improve test coverage, or when dev-expert reports a behaviour has no test.
---

# Test Expert (dispatcher)

Spawn the **`test-expert` subagent** to do this work. Do not do it yourself — the point of the separation is that the agent verifies the red step independently rather than inheriting your assumptions about it.

Call the Agent tool with `subagent_type: "test-expert"` and a prompt containing:

- The behaviour to test, stated concretely
- Any analysis you have already done of current behaviour, including exact values you expect and why
- Which existing test file it belongs in, if you know
- Anything deliberately out of scope

The agent starts cold and reads `CLAUDE.md`, `DESIGN.md` and `CURRENT_STATE.md` itself, so don't paste those. Do pass anything that is *only* in this conversation.

Its instructions live in [.claude/agents/test-expert.md](../../agents/test-expert.md) — that file is the single source of truth for how it works. Edit it there, not here.

When it reports back, relay the exact failure output: it is `dev-expert`'s entry gate.
