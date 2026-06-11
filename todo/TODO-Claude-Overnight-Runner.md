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
pipeline below -- no deviation, no reordering, no skipped stages. The run ends
only when every domain is complete or a hard-stop condition fires.

## Cursor

- **Start:** `todo/01-boot-platform/TODO-11-interrupt-timer-arch.md`
- Everything before the cursor (domain `00-infrastructure`, and
  `01-boot-platform` TODO-01 through TODO-10) is DONE -- do not revisit except
  when an XREF from active work lands a concrete item there.
- **Resume rule:** on session start, the cursor is the first TODO file (in
  traversal order, from the start point) that is not yet 100% complete; within
  it, the first section not yet shipped-and-reviewed.

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

1. **Validate** -- `Skill(validate-todo-file)` on the file. Fix structural
   findings before proceeding.
2. **Gap analysis** -- `Skill(gap-audit-todo)` on the file (includes the
   mandatory `codex-gap-audit` secondary pass). Land the resulting TODO edits
   before touching code.
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

- **NO ChromeMCP / browser automation. Ever.** Impossible OS has its own
  smoke-test infrastructure: `bash scripts/test-smoke.sh` (boot-to-userspace),
  `bash scripts/test.sh` (unit suites), `bash scripts/build.sh` (build).
  Browser verification gates in the runner are satisfied by these; waive any
  browser-specific gate with that justification rather than reaching for
  ChromeMCP.
- **Smoke testing matters.** It is built into the section pipelines
  (implement-todo-section step 16, review verification) -- run it after every
  boot-path change; do not skip it to save time.
- **Halt on first hard failure** (build break that survives a fix attempt,
  test regression with unclear root cause, Codex dispatch infrastructure
  down, commit-gate deadlock): per `feedback_overnight_halt_on_error`,
  diagnose and fix the root cause; if the root cause cannot be established,
  PAUSE the run and leave a clear note in this file's Run Log -- never skip
  past, never paper over with workarounds.
- **Full quality pipeline every section** -- all Codex dispatches, domain
  code-quality gates, unit tests, stamps. No "straightforward section"
  exceptions (`feedback_no_corner_cutting`, `feedback_never_skip_review`).
- **No scope inflation:** gap-audit edits go into TODO files as checklist
  items; do not implement gap findings inline outside the section pipeline.
- All other repo doctrine (CLAUDE.md, memory feedback) applies unchanged.

## Run Log

> Append one line per session: date, cursor at start, cursor at end, sections
> shipped, pauses/blockers. Keep entries to one line each.

- (no runs yet)
