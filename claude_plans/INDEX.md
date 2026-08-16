# Where things live, and where to write a new one

**This file has no content of its own.** It says which document owns what, so a fact gets recorded once instead of four times. Read it before adding to any of the others.

## Why it exists

The project's recurring bug is *a wrong answer that looks plausible*, and its documents have the same disease. In one week, three separate facts went stale in four or five places at once:

- Case 22's LP refusal was stated in DESIGN, CURRENT_STATE, PROMOTION_DESIGN, WALL_CASES.html and two test comments. The mean-strength re-anchor moved the data underneath it and **every one of the seven went wrong together**, silently, for a day — while a slice was planned around the blocker it named.
- Case 9's governing axis was recorded as "squeezed-edge compression" in DESIGN §6, CURRENT_STATE, TRAPS and a test comment. It is Mohr-Coulomb **shear**. The compression reading is 13× under; all four were wrong.
- The overturning guard's bond basis was 0.6 in DESIGN §5.7 and profile-read in CURRENT_STATE, in the same tree, **contradicting each other**.

None of these were caused by carelessness in the moment. They were caused by **transcribing a measurement into prose in several places**, after which nothing checks the prose ever again.

## The rule that prevents most of it

> **A number that a test pins is quoted in AT MOST ONE document. Everywhere else names the test that pins it.**

Write *"wall-01 answers at the λ\* pinned in `WallsAndLadders`"*, not *"wall-01 answers at 272.20488981561004"*.

The reason is mechanical rather than stylistic: **a test file is the only place a number is re-checked against reality on every run.** A figure in markdown is checked by nobody, so the moment the data moves it becomes a confident lie with no failure mode. When you genuinely need the value in prose — a ruling that turned on it, a design argument — quote it in the *one* document that owns that kind of statement, and cite the pin beside it so a reader can tell whether it still holds.

Corollary, learned 2026-08-15: **a pin only notices a change that goes through the code it calls.** A repair implemented *beside* the thing it repairs is invisible to every pin on the original — so a forward hazard that names a *behaviour* is sound, and one predicting *a specific test will fail* is a prediction about an implementation nobody has chosen yet.

## Who owns what

| Document | Owns | Must **not** contain |
|---|---|---|
| [../CLAUDE.md](../CLAUDE.md) | How to work here: build and test commands, the subagent pipeline, house style, the hard rules | Findings, measurements, project state. It is deliberately short — resist adding to it |
| **INDEX.md** (this file) | Which document owns what; the one-quote rule | Anything else |
| [DESIGN.md](DESIGN.md) | The model and why it is shaped that way; constants with citations; §6's anchor table; §7's evolution path; **§8's dated rulings** | Outstanding work (→ CURRENT_STATE); measured figures a test pins, beyond the one quote a ruling needs |
| [CURRENT_STATE.md](CURRENT_STATE.md) | **Only what is not done.** Open items, specified-but-unbuilt tests, deferred findings with enough context to act cold | Completed-work narrative, measurement archives, history. Finishing something means **deleting** its entry, not appending an outcome to it |
| [TRAPS.md](TRAPS.md) | Live footguns in the tools, engine and conventions; the **mutation registry** with each signature | Physics or model reasoning (→ DESIGN) |
| [PROMOTION_DESIGN.md](PROMOTION_DESIGN.md) | Evolution step 4's design, its measurements and its decisions (§12) | Copies of DESIGN §8 rulings — cite them |
| [LEVELS.md](LEVELS.md) | The playable levels and how to reach them | Verdict reasoning (→ WALL_CASES / DESIGN) |
| [WALL_CASES.html](WALL_CASES.html) | The wall catalogue as a reader-facing page: each case, its verdict, the hand statics | Solver internals; anything the acceptance test is the authority on |
| **Test file headers** | **The measurements themselves** — prediction beside measurement, and the reasoning for the assertion choice | — (this is the source of truth; everything else cites it) |

## Where does this fact go?

- **A number a test now pins** → the test's own header. Cite it elsewhere; do not restate it.
- **A decision only a human could make** → DESIGN §8, dated, with the evidence and the cost. Rulings do not go stale; they are historical by nature.
- **Something that is not done** → CURRENT_STATE, with enough context to pick up cold.
- **Something that just bit you and will bite again** → TRAPS, with the signature or the reproduction.
- **A mutation and what it fires** → TRAPS' registry, with the counting method stated.
- **Why the model is shaped this way** → DESIGN.
- **Something now finished** → *delete* its CURRENT_STATE entry. If the reasoning is worth keeping, it belongs in DESIGN or the test header, and git holds the rest.

## The maintenance habit

When a measurement moves, grep for its digits across `claude_plans/` and `Source/` before believing the tree agrees with itself. Every incident above would have been caught by that one command — which is why the one-quote rule exists: it makes the grep return one hit instead of seven.
