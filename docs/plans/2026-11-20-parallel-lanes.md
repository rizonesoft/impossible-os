# Parallel Lanes: Two Claude Lanes and Two Neon Lanes

**Goal:** main runs alone, or with any mix of three extra lanes (userland, neon-a, neon-b), started and stopped from the desktop whenever the operator wants, with no git, file, push or resource clash, and with every fault visible. This file holds the design decisions; the work is tracked in the TODO system.

**Status:** filed 2026-11-21 as [TODO-08](../../todo/00-infrastructure/TODO-08-automation-hardening.md) sections 48 to 59, placed first in Phase 0 of the implementation plan (operator decision), so the run builds them like any section. Brainstormed and decided 2026-11-20.

## Decisions

Each line records what was decided and what reversing it costs.

- **Lanes.** main (Claude, primary tree, everything no other live lane owns), userland (Claude, `user/`, `src/apps/`, `src/shell/`), neon-a (agy `gemini-4-neon-high`, `scripts/inventory/`, `docs/design/inventory/`, TODO-17 and the reference VM, later ready UI surfaces), neon-b (agy, reports only, never commits). Reversal: edit `.conclave/lanes.toml`.
- **Main first.** A lane's start refuses unless main is running, because main's heartbeat watches the lanes. Stopping main asks, then stops running lanes at safe points first. Reversal: give the lanes their own watcher.
- **Dynamic ownership.** A lane reserves its directories only while it is live; when a lane is off, main takes its sections. Claim refs keep a half-done section with its lane across a stop. Reversal: static ownership in `lanes.toml`.
- **Claude designs.** Claude keeps the UI engines, the component library, the consistency-ledger rules and the frosted glass. Neon builds a UI surface only after the inventory checker marks it ready, with a screenshot diff against the capture as the done condition. Records for icons and text never go into Neon tasks. Reversal: drop the ready status, and Neon's UI work goes back to Claude.
- **Neon plays to its strengths:** the 1M-token context (prep packages, roadmap and doc sweeps, spec digests) and reading images and video (capture, redaction check, visual comparison). It never designs, never makes the final SMP or ABI call, and never reviews. Astra reviews every lane; Claude never reviews.
- **Always busy.** The queue order is: Claude's blocked requests, then prep packages for the Claude lanes' next three sections, then lane sections and ready surfaces, then standing sweeps (which never run out). Neon-b only produces reports.
- **Neon billing is a subscription, with no spend cap.** Tokens are recorded for sizing only.
- **Neon-b starts after the pilot** shows that main plus neon-a ship with no clashes.
- **Extra lanes are light.** Each runs under a systemd scope with memory and CPU limits. Heavy jobs (build, suites, smoke matrix, QEMU) are refused outside the push lock and run once per section at push time through bosun's single slot. Reversal: raise the limits in `lanes.toml`.
- **Astra reviews it** section by section through the normal pipeline (plan, round, performance, completion); Claude never reviews.
- **Faults are reported, never auto-restarted,** per the global rule that a run never restarts itself. Main keeps its existing heartbeat restart.
- **Policy.** The CLAUDE.md "Autonomous-agent boundary" becomes "Two engines, one gate set": only these operator-started lanes may commit. Reversal: revert the paragraph and stop the extra lanes.

## Limits

- Two Claude lanes use the Claude subscription about twice as fast; a usage limit stalls main and userland together. The heartbeat reports it as a stall, but can't prevent it.
- An extra lane finds out its build is broken only at push time, then fixes and queues again. Light checks (ruff, script tests, a single-file compile, staged lint) catch most of these earlier.
- Neon UI surfaces wait until TODO-18 (UI engines) and TODO-19 (component library) are built.

## Where the work lives

All in [TODO-08](../../todo/00-infrastructure/TODO-08-automation-hardening.md):

| Section | Work |
| --- | --- |
| 48 | Policy: CLAUDE.md, `ai-system.md`, `AGENTS.md`, `GEMINI.md` |
| 49 | Lane map (`.conclave/lanes.toml`), live records, the pre-commit lane check, triage skips |
| 50 | Claims as `refs/claims/<address>` on origin |
| 51 | The push queue flock, held through CI green |
| 52 | Light lanes: systemd scopes, heavy jobs only inside the push lock, agy deny settings |
| 53 | `scripts/lane.sh`, the worktrees, the userland lane |
| 54 | `scripts/run.sh` reads the lanes (frozen file, `**Needs:** attended`) |
| 55 | The Neon driver and task queue |
| 56 | Heartbeat alerts for every lane fault |
| 57 | Nine desktop shortcuts |
| 58 | Ready UI surfaces and the screenshot diff |
| 59 | The pilot (`**Needs:** operator`) |

The review-machinery ideas measured on 2026-11-20 are TODO-08 sections 60 to 67: atomic receipts, a P2 with a failing test, blocking per path, a redesign pass on repeated findings, repair-only re-dispatches, tools in front of lenses, lens yield, and the push wait overlapping the next plan review.

## Operator runbook

Check progress at any time with `grep -A11 "Two engines, one gate set" todo/implementation-plan.md`, which prints the twelve lanes rows (the box column: `[x]` done, `[/]` started or parked, `[ ]` not started).

### Stage 1: the run builds the lanes

1. Start the run with the "Impossible OS - Start run" shortcut. It begins with the first lanes section, "Two engines, one gate set".
2. Leave it working through §48 to §58. Start no lane yet: until §53 there is nothing to start, and until §56 nothing watches a lane.

### Stage 2: the frozen-file step (§54)

3. Once §53 shows `[x]`, the run parks §54 (`scripts/run.sh` is frozen) and goes on with §55. Open the "Impossible OS (Claude)" shortcut and say: "Do the section The run knows its lanes". That session works in a separate worktree and pushes to main; the run keeps going.

### Stage 3: get ready

4. Wait until §48 to §58 all show `[x]`. The run then parks §59 (the pilot is yours) and works on the review sections §60 to §67, which is expected.
5. Open the "Antigravity (WSL)" shortcut once and confirm agy is signed in, then close it.
6. Run `bash scripts/run-desktop.sh install` once. The desktop gains Start, Watch and Stop for Userland, Neon A and Neon B.

### Stage 4: the pilot, one lane at a time

7. With main running, open "Neon A - Start". Keep "Neon A - Watch" open beside it.
8. Wait until Neon A has shipped one section (its push green in CI) with no toast and no fault in `bash scripts/lane.sh status`.
9. Open "Userland - Start" and wait for one clean userland section the same way.
10. Open "Neon B - Start". It commits nothing; its first reports appear in `~/.local/state/impossible-os/agy/done/`.
11. When all four lanes have run clean, open the "Impossible OS (Claude)" shortcut and say: "The lanes pilot passed, close the Lanes pilot section".

### Every day after that

- Always start main first; start any extra lane whenever you want. A lane refuses to start without main and its tab says why.
- Stop a single lane with its Stop shortcut; it never stops mid-push. Stopping main asks about running lanes first.
- A toast means a lane fault. Open that lane's Watch shortcut, read the reason, then fix it or ask Claude. Lanes are never restarted for you.
