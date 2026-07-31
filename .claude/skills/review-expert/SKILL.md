---
name: review-expert
description: Review completed DestructionGame work before it is called done. Use after dev-expert has a green test, before committing, or when asked to review a change, a test or a design decision. Expected to reject work, not rubber-stamp it.
---

# Review Expert (dispatcher)

Spawn the **`review-expert` subagent** to do this work. Do not do it yourself — a reviewer that shares the author's context inherits the author's blind spots, which is precisely what this gate exists to catch.

Call the Agent tool with `subagent_type: "review-expert"` and a prompt containing:

- What changed and what drove it
- The claimed suite result — the agent will re-run it, and treating your summary as a claim rather than evidence is the point
- Anything you already know is imperfect or deliberately deferred, so it can judge whether deferring was right rather than rediscovering it as new

The agent has **no write access**, deliberately. It reports findings; you act on them. That stops a reviewer quietly fixing a problem instead of surfacing it.

Its instructions live in [.claude/agents/review-expert.md](../../agents/review-expert.md) — the single source of truth. Edit it there, not here.

Expect findings. Act on the blocking ones before considering the work done, and log deferred ones in `CURRENT_STATE.md` rather than letting them evaporate.
