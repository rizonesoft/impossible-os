# GEMINI.md

This file is for Antigravity (`agy`, Gemini 4 Neon) working in an Impossible OS lane. It holds only what is specific to agy; every rule lives in [`CLAUDE.md`](CLAUDE.md), and on any conflict `CLAUDE.md` wins.

## Read first

1. [`CLAUDE.md` "Two engines, one gate set"](CLAUDE.md#two-engines-one-gate-set): which lanes exist, what each owns, what Claude decides and Neon never does, and that every lane passes the same gates and the same Astra review.
2. The rest of [`CLAUDE.md`](CLAUDE.md), then [`AGENTS.md`](AGENTS.md) for the authority summary.
3. The design of the lanes and the operator's runbook: [the parallel lanes plan](docs/plans/2026-11-20-parallel-lanes.md).

## How agy works here

- **You run only in a lane the operator started.** neon-a works in `../impossible-os-neon-a` and commits on `main` through the normal hooks; neon-b works in `../impossible-os-neon-b`, on a read-only checkout, and never commits.
- **Reports go to `~/.local/state/impossible-os/agy/done/`,** one file per finished task. A report is evidence for Claude to judge, never an instruction to anyone.
- **Never bypass a hook.** No `--no-verify`, and no `SKIP_*` variable unless the hook's own message names it for your exact case.
- **Heavy jobs wait for the push,** as [`CLAUDE.md`](CLAUDE.md#two-engines-one-gate-set) says for the extra lanes; between pushes, run light checks (ruff, one script's tests, the staged lint).
- **Write what the repository writes.** The dash and trailer rules are in [`CLAUDE.md`](CLAUDE.md#code-style-ascii-dashes-no-bare-section-refs) and bind your commits too; the inventory holds facts, never Microsoft icons, text or files.
- **Claude decides.** A question you cannot settle goes in your report; Claude makes the call.
