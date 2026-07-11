---
$schema: ../docs/infrastructure/todo-metadata.schema.json
schema_version: 1
id: claude-overnight-runner
domain: todo
status: active
title: "TODO-Claude-Overnight-Runner -- OS Completion Driver (runner doctrine, not an implementation TODO)"
---

# TODO-Claude-Overnight-Runner -- OS Completion Driver

> **Runner doctrine file, not an implementation TODO.** This file is the control
> program for the unattended overnight runner: it defines the traversal order,
> the per-file pipeline, the per-section pipeline, and the hard rules. The
> runner's only state is the repo itself (checklists, stamps, commits); this
> file is re-read at the start of every session and after every compaction.

## Mission

Drive every TODO file under `todo/` to completion, in order, using the exact
pipeline below -- no deviation, no reordering, no skipped stages. The runner is
a **fixpoint loop**: it sweeps the whole repo in traversal order, then sweeps
again, because most blockers are temporal (a section blocked on a later domain's
section completes on a later pass once that section ships). The run ends only
when a **complete pass makes zero progress** (nothing newly shipped, no deferral
newly cleared) -- at which point everything autonomously-doable is implemented,
tested (KVM/TCG + unit), reviewed, and committed, and the only remainder is a
short human punch-list (bare-metal / WHPX sign-off, genuine product decisions).

## Cursor

- **Current (2026-06-12, run 4):** `todo/01-boot-platform/TODO-12-early-entropy-random-seed.md`.
  Stages 1-2 (validate + gap-audit) and §1-§3 + §5 are done AND reviewed
  (§5 review shipped 5ccdd14d). TODO-13 §2 TPM transport pulled forward and
  shipped + reviewed (8ae51bf1 + review stamps) per user direction, so §4 is
  UNBLOCKED and now SHIPPED + REVIEWED (§4: ee546157 + review stamps).
  **USER AUTHORIZED Monocypher vendoring (2026-06-12):** the dependency
  addition for `02-kernel-core/TODO-03` §5 is approved -- do not re-ask.
  Run-5/6 work queue, in order:
  1. **`02-kernel-core/TODO-03` §5** -- DONE: Monocypher 4.0.2 + kernel
     CSPRNG + `NtGetRandom` shipped (99910014) + reviewed (21168666).
     (TODO-03 gets its own validate/gap-audit when the domain cursor
     reaches it; only §5 was pulled forward.)
  2. **TODO-12 §6** -- DONE (run 6): seed-file carryover shipped
     (08340720) + reviewed (23b3b611); FAT32 durability/coherence/LFN
     fixes landed with it; follow-ups filed in `05-storage/TODO-04`
     §4/§6/§15 + `01-boot/TODO-24` §5.
  3. **TODO-12 §7** -- DONE (run 8): boot_info seed handoff shipped
     (dff35172) + reviewed (c1da9924); digest-chained first-seed
     handoff, producer-identity frame release, FLAG_VALID retire
     semantics, NVRAM verify budget, phase3 early-counter anti-replay.
  4. **TODO-12 §8** -- DONE (run 8): early CSPRNG seeding shipped
     (4ce71522) + reviewed (d364441e); named init point, CSPRNG-owned
     credited class, unconditional key-gen gate, classified fill.
  5. **TODO-12 §9** entropy diagnostics + policy gates. <- NEXT
  6. **§10 tests**, then `complete-todo-file` closure.
- TODO-11 is closed: §1-§6 + §8-§10 shipped and reviewed; §7 stays `[/]`
  blocked-with-XREF on `03-memory-concurrency/TODO-07` §3; sweep c4e92774.
- **Original start:** `todo/01-boot-platform/TODO-11-interrupt-timer-arch.md`
- Everything before the cursor (domain `00-infrastructure`, and
  `01-boot-platform` TODO-01 through TODO-10) is DONE -- do not revisit except
  when an XREF from active work lands a concrete item there.
- **Resume rule:** the cursor is computed, not hand-maintained. On session start
  the runner runs the triage oracle `python3 .claude/hooks/sequencer_triage.py
  --next` (graph-truth over `build/todo-cache.json` + section review stamps),
  which returns the first NEEDS_WORK file in traversal order; within
  it, `--classify <file>` gives the first section not shipped-and-reviewed,
  plus a `stages_1_2_done` flag (from the file-preamble `> **Validated:**` /
  `> **Gap-audited:**` stamps) that Stage 0 of the per-file pipeline uses to
  skip re-validating / re-gap-auditing an already-processed file.
  DONE files (every section `[x]`/`[/]` with BOTH Verified + Quality-reviewed,
  or Deferred-parked) are skipped. A shipped `[x]` section missing either stamp
  is NOT done -- the oracle classes it NEEDS_WORK (there is deliberately no
  DONE_UNSTAMPED class), and Stage 3 runs `review-todo-section` on it via the
  already-complete-section branch. The Cursor block below is a human-readable
  convenience log, NOT the source of truth -- the oracle is.

## Traversal Order

1. Files within a domain: ascending `TODO-NN` numeric order (skip `INDEX.md`
   and any non-TODO doctrine files, including this one).
2. Domains: ascending numeric order -- `01-boot-platform` ->
   `02-kernel-core` -> `03-memory-concurrency` -> `04-drivers-hardware` ->
   `05-storage-filesystems` -> `06-desktop-foundation` -> `07-networking` ->
   `08-graphics-ui` -> `09-desktop-shell` -> `10-platform-services` ->
   `11-apps` -> `12-user-platform-sdk` -> `13-tools-accessories` ->
   `14-host-tools` -> `15-installer-release` -> `16-architecture-ports` ->
   `17-polish-hardening` -> `18-future-research`.
3. End of a domain (e.g. `01-boot-platform/TODO-29-boot-perf-health-observability.md`
   done) -> first file of the next domain (e.g.
   `02-kernel-core/TODO-01-kernel-init-sequencing.md`).

## Per-File Pipeline (exact sequence, no deviation)

For each TODO file at the cursor:

0. **Lifecycle check (skip Stages 1-2 when already done).** Run
   `python3 .claude/hooks/sequencer_triage.py --classify <file>` and read its
   `stages_1_2_done` field. The file-preamble stamps `> **Validated:**` and
   `> **Gap-audited:**` (above `> **Goal:**`) record that Stages 1-2 already
   ran on a prior pass. If `stages_1_2_done` is true AND no `## N.` section has
   been ADDED since (a new XREF'd `[ ]` item inside an EXISTING section does
   NOT count -- it already came from a gap-audit elsewhere), SKIP Stages 1-2
   and go straight to Stage 3. This is the guard against the fixpoint loop
   re-validating / re-gap-auditing a mature file every sweep (incident
   2026-06-21). Re-run Stages 1-2 only when the stamps are absent or a
   genuinely new section appeared.
1. **Validate** -- `Skill(validate-todo-file)` on the file. Fix structural
   findings before proceeding. On completion, write/refresh the file-preamble
   stamp `> **Validated:** <date> | <one-line basis>`.
2. **Gap analysis** -- `Skill(gap-audit-todo)` on the file (includes the
   mandatory `codex-gap-audit` secondary pass; its Phase 2.0 maturity gate keys
   off these same signals). Land the resulting TODO edits before touching code.
   On completion, write/refresh the file-preamble stamp
   `> **Gap-audited:** <date> | <one-line basis>`.
3. **Sections, in order.** For each `## N.` implementation section, first to
   last:
   - Section already complete (all items `[x]`, marked `[x]` in the
     Implementation Order table) -> `Skill(review-todo-section)`.
   - Section not complete -> `Skill(implement-todo-section)`, which chains
     into `review-todo-section` at step 20 as usual.
   - **Commit AND push after every section** (one atomic act per
     `feedback_commit_push_workflow`). Never batch sections into one commit.
4. **File closure** -- when all sections are shipped: `Skill(complete-todo-file)`
   loose-end sweep, then advance the cursor to the next file.

**File-complete criterion:** 100% of sections shipped and reviewed. A small
number of `Deferred:`/`Accepted:` items with concrete XREFs is acceptable ONLY
when implementation is genuinely blocked (missing infrastructure owned by a
later TODO, hardware-only validation); "hard" or "tedious" is not blocked.

## Hard Rules

- **Session-exit policy: the work unit is the ENTIRE queue, not one
  section -- and the runner NEVER stops or disarms itself.** Finishing a
  section (ship + review + push) is NOT a reason to run
  `overnight-runner finish-check` / `handoff` or to final-answer. After every
  section: update the cursor, then IMMEDIATELY start the next section in the
  queue. There is NO voluntary session-exit on a blocker of ANY kind:
  - A **user-decision / operator-reserved item** (a stop-and-ask boundary) is
    NOT a stop and NOT a disarm. Resolve it by the three-tier answer order
    (adopted 2026-07-03): (1) if `todo/answers.md` carries an
    `A: (operator, DATE)` answer matching the TODO + question, apply it as the
    decision; (2) a LOW-RISK question (no money/permissions/data/ABI impact)
    may take a conservative logged default -- record it in `todo/answers.md` as
    `A: (proposed default)` so the operator can confirm or veto; (3) otherwise
    DEFER the item (`[/]` + a Deferred stamp whose line contains
    `awaiting-answer` + an XREF naming the decision) and ADVANCE. The
    `awaiting-answer` token makes the deferral RECOVERABLE: the 3-state oracle
    classes the section BLOCKED (never falsely DONE), the launcher's heal probe
    retries once per `todo/answers.md` edit, and `collect-questions.py` writes
    the consolidated operator punch-list to
    `.claude/overnight/questions-for-operator.md` at each lifecycle exit.
    Terminal (won't-do / out-of-scope-here) deferrals keep the plain Deferred
    stamp + XREF as before -- no awaiting token. A single reserved decision in
    one section must never strand the other 85 TODO files. (Incident
    2026-06-16: the runner read a stale "user reserved this" blocker and ran
    `--disarm`. Both the stale-state and the self-disarm paths are now closed;
    the behavioral rule is: defer and advance, full stop.)
  - A **hard failure** is handled by the defer-and-escalate rule below (defer +
    advance, never halt).
  - **External death** (usage limit / API error) is not a choice; the watchdog
    relaunches and the run resumes.
  The runner must NEVER run `arm-sequencer.sh --disarm`,
  `run_phase_guard.py clear`, `run_phase_guard.py phase FIXPOINT`,
  `systemctl stop`, or otherwise tear down its own run -- disarm is a
  HUMAN-ONLY operation, and `run_phase_guard.py` hard-blocks self-teardown from
  the headless run. Do NOT clear the overnight-guard state between sections.
  The ONLY permanent stop is an oracle-verified `run_phase_guard.py fixpoint`
  (every section in every domain DONE or deferred-with-XREF; it writes the
  FIXPOINT sentinel and auto-disarms the watchdog) or the human's `--disarm`.
  The watchdog relaunches any death; mid-queue voluntary exits accomplish
  nothing.
- **"The run stays active" vs "the worker context rotates" (2026-07-11).**
  Two session endings are NOT stops -- in both, the run stays ARMED, the
  durable cursor (`sequencer-run.json`) carries all state, and a fresh worker
  session resumes the same run. They are the only guard-sanctioned session
  endings besides fixpoint/disarm:
  - **Structural wait (WAITING_REVIEW).** When a background review/agent
    verdict is the only thing between you and the next action, declare it
    (`run_phase_guard.py wait <timeout_s> <reason> <artifact> <pattern>
    [...]`) and end the session. A non-model watcher wakes a fresh session
    exactly once when every artifact matches its completion pattern (watchdog
    tick as fallback; expiry wakes you to handle the timeout). Zero model
    turns are spent holding; no review is skipped -- the woken session reads
    `woke_from_wait` from `status` and MUST receive the verdict before
    anything else.
  - **Verified rollover.** After a section is implemented, reviewed, pushed,
    and fully stamped, `run_phase_guard.py rollover` machine-verifies the
    checkpoint -- clean tracked tree, zero unpushed commits, todo-graph
    rebuild green, content-bound build receipt valid for the current tree,
    no unreceived Codex review, no declared wait -- and only then permits ONE
    clean session end; the watcher relaunches a fresh worker context
    immediately. A REFUSED rollover lists unfinished work: finish it and
    continue in-session. Rollover exists because a fresh context outperforms
    a long-tail one; it is never a way to leave work behind (the gates make
    that impossible).
- **NO ChromeMCP / browser automation. Ever.** Impossible OS has its own
  smoke-test infrastructure: `bash scripts/test-smoke.sh` (boot-to-userspace),
  `bash scripts/test.sh` (unit suites), `bash scripts/build.sh` (build).
  Browser verification gates in the runner are satisfied by these; waive any
  browser-specific gate with that justification rather than reaching for
  ChromeMCP.
- **Smoke testing matters.** It is built into the section pipelines
  (implement-todo-section step 16, review verification) -- run it after every
  boot-path change; do not skip it to save time.
- **Hard failure: defer-and-escalate, never halt-the-run, never ship broken.**
  On a hard failure (build break surviving the skill's fix loop, test
  regression with unclear root cause, Codex infra down, commit-gate deadlock):
  first diagnose + fix the root cause via the skills' bounded fix loops. If it
  STILL fails, roll back (commit nothing, clean tree) and DEFER the section with
  the captured diagnostic, then advance -- a later fix elsewhere may unblock it
  on the next pass. A failure that reproduces identically across K=3 passes
  escalates to the residual punch-list (Run Log + the section's Deferred stamp)
  for human attention. The run never stops on a single failure (that defeats the
  come-back-in-a-month goal) and never papers over with workarounds or ships
  broken code (`feedback_no_bandaids`, `feedback_fix_root_causes`). Deferral uses
  the EXISTING machinery (`[/]` + Deferred/Accepted stamps, todo-graph
  `deferred`, `complete-todo-file` sweep), not a parallel ledger. [Supersedes the
  prior halt-and-pause rule for unattended fixpoint runs, per the 2026-06-13
  sequencer design (retired from the tree 2026-07-03; git history:
  `docs/superpowers/specs/2026-06-13-overnight-sequencer-design.md`).]
- **Full quality pipeline every section** -- all Codex dispatches, domain
  code-quality gates, unit tests, stamps. No "straightforward section"
  exceptions (`feedback_no_corner_cutting`, `feedback_never_skip_review`).
- **No scope inflation:** gap-audit edits go into TODO files as checklist
  items; do not implement gap findings inline outside the section pipeline.
- All other repo doctrine (CLAUDE.md, memory feedback) applies unchanged.

## Soft Cost SLOs (advisory targets -- never quality-cutting budgets)

Efficiency targets the run self-checks against; breaching one is a signal to
fix the WORKFLOW, never to skip a gate, review, or test:

- **Zero inference requests while waiting** -- background verdicts go through
  the structural wait (`run_phase_guard.py wait`), not held-open sessions.
- **At most one full load of each skill per section** (compaction excepted) --
  re-reads are slices.
- **No successful build/test through the main session** -- green mechanics run
  via `scripts/overnight/preflight.py`, `checks-runner`, or
  `scripts/overnight/run-artifact.sh`; only failures earn main-session
  attention (via diagnostic-digester first).
- **Checkpoint before context exceeds ~200-250K tokens when safely possible**
  -- prefer a verified `rollover` at the next section boundary over drifting
  into compaction.
- **Track main-loop cache-read + output tokens per shipped commit** --
  `scripts/overnight/section-cost-report.py <metrics.jsonl>` normalizes by
  changed LOC / files / commits; compare runs ONLY on normalized numbers.
- **Alert on repeated unchanged receipts** -- an `agent-cache` HIT or an
  identical re-dispatch over unchanged content means the workflow re-asked a
  settled question; `scripts/overnight/offload-report.py` and the cache-hit
  block messages surface these.
- **Unchanged-input retry rule (soft):** no build, review, agent dispatch, or
  failed edit is repeated unless its relevant input hash changed (receipts,
  agent cache, failure ledger, finding ledger are the oracles) or an explicit
  one-line reason is recorded for the deliberate re-run.
- **Agent scorecard -- score by NET savings, not dispatch count.** Per agent
  type, periodically assess: main-session requests avoided, main cache-read
  reduction, Sonnet cost added, share of claims the main session rejects at
  verification, re-dispatch frequency (cache should absorb repeats), and
  misses later caught by Codex. Retire or revise an agent that adds more
  context than it removes. Inputs: metrics sidecar (main vs sidechain split),
  `offload-report.py`, `section-cost-report.py`, agent-cache hit logs.
- **Decision capsules (EXPERIMENTAL, A/B-gated, default OFF).** Protocol for
  a future A/B leg only -- never a default: a short high-effort session
  produces a content-bound implementation/review plan (bound to the section
  manifest's blob hashes); a fresh medium-effort worker executes the
  mechanical portion; high-effort handles security/ABI decisions and final
  finding triage. Run it only against a measured baseline (normalized by
  `section-cost-report.py`) -- two sessions can cost MORE when the plan is
  weak; abandon the leg if quality or cost regresses.

## Enforcement & Scheduling

This doctrine is not just guidance: it is hard-enforced. Architecture is
**repo schedules, repo decides** (2026-06-13 sequencer design, in git history):

- **Scheduler (repo-vendored, `scripts/overnight/`):** systemd main + watchdog
  timers, headless `claude` launch, usage-limit snooze, linger-survival,
  `flock`. Vendored 2026-06-16 from the rizonetech `overnight-runner` plugin and
  de-coupled from it -- the repo owns `overnight-arm.sh` + `overnight-launch.sh`
  so there is exactly ONE armed path (no competing unguarded plugin flow), no
  dependency on a user-cache plugin version, and ChromeMCP is gated off at the
  source for kernel runs. The launcher bootstraps `Skill(overnight-sequencer)`
  directly (no plugin slash command). The **watchdog is pure failover at
  `*:0/10`**: a tick is a no-op if a run is alive (flock); it relaunches only
  when a run died (crash / usage-limit / kill) and the run has not cleanly
  finished. On an oracle-verified fixpoint the runner **auto-disarms its own
  timers**, so it stops for good (no spin-after-done).
- **Brain (this repo, `.claude/`):** `sequencer_triage.py` (cursor oracle),
  `run_phase_guard.py` (a PreToolUse + Stop hook that hard-blocks any tool call
  outside the current phase's allow-list, and blocks `AskUserQuestion` entirely --
  unattended means decide-or-defer), and the `overnight-sequencer` skill that
  drives the per-file / per-section pipeline above. The within-section gates
  (design-review, adversarial, section-commit, review-required, skill-step-block)
  keep running inside `implement/review-todo-section`; the phase-guard governs the
  outer sequence. Run mode is always `bypassPermissions` (a weaker mode stalls an
  unattended run); it is safe precisely because the run physically cannot skip a
  phase, ship un-reviewed code, or commit a failing build.
- **ChromeMCP is off at the systemd-unit level for this repo only** (per-unit env
  on impossible-os's units), so the "NO ChromeMCP. Ever." rule is enforced before
  `claude` even launches, without affecting any other project's overnight runs.
- **FIXPOINT is machine-verified, not asserted.** The only path to a permanent
  stop is `run_phase_guard.py fixpoint`, which rebuilds the todo-graph and
  REFUSES unless `sequencer_triage.py --next` returns DONE (zero remaining work).
  Until then `Stop` is blocked, so the run cannot finish early; a false fixpoint
  is impossible. Only a verified fixpoint auto-disarms the watchdog.

## Run Log

> **Archived (2026-07-11):** entries live in [docs/overnight/run-log.md](../docs/overnight/run-log.md) -- the doctrine file is re-read every session start and after every
> compaction, and 64 KB of append-only history was riding along on each of those reads. APPEND new one-line entries (date, cursor at start/end, sections shipped,
> pauses/blockers) to the ARCHIVE file, never here. `runner_status.py` reads recent decisions from the archive.
