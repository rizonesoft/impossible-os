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

- **Session-exit policy: the work unit is the ENTIRE queue, not one
  section.** Finishing a section (ship + review + push) is NOT a reason to
  run `overnight-runner finish-check` / `handoff` or to final-answer. After
  every section: update the cursor, then IMMEDIATELY start the next section
  in the queue. A session may end ONLY on: (a) a user-decision blocker
  (stop-and-ask boundary -- record the question in the cursor first), (b) a
  hard failure per the halt-on-error rule below, (c) every remaining section
  in every remaining domain blocked-with-XREF, or (d) external death (usage
  limit / API error -- not a choice). `finish-check` + `handoff` run ONLY in
  cases (a)-(c). Do NOT clear the overnight-guard state between sections.
  The hourly watchdog timer relaunches the run after any death; mid-queue
  voluntary exits defeat the runner's purpose.
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

- 2026-06-11 16:36 SAST: run 1 -- TODO-11 validate + gap-audit done, sections 1-4 worked; stopped at section 5 (Dynamic IRQ Registration API) on Claude usage limit; uncommitted section-5 work left in tree; resume armed for 20:47.
- 2026-06-12 02:35 SAST: run 1 resumed -- TODO-11 §5 + §6 implemented + reviewed, §7 reviewed ([/] blocked on D03T07§3), §8/§9/§10 review passes shipped (98592c32, 7ea3a01f, 80328e01), loose-end sweep c4e92774; cursor advanced to TODO-12-early-entropy-random-seed.md.
- 2026-06-12 ~02:35-04:48 SAST: run 2 (same session) -- TODO-12 validate + gap-audit + §1-§3 shipped; §4 filed blocked-with-XREF (a98dca94); died mid-§5 on transient API 500, staged §5 work left in tree; resume armed, §4 re-opened per user direction.
- 2026-06-12 ~07:00 SAST: run 3 (interactive, cut short) -- preflight caught build break at HEAD (half-committed peek->drain refactor); drain implemented + tests migrated + committed (dd19d586), build OK, security suite green; user redirected to headless timer; §5 review + §4 re-open queued for run 4.
- 2026-06-12 ~07:00-09:45 SAST: run 4 -- TODO-12 §5 reviewed (5ccdd14d, Codex 4x); TODO-13 §2 TPM2 transport pulled forward, shipped + reviewed (8ae51bf1 + c3779b6d, Codex 9x); TODO-12 §4 TPM RNG shipped + reviewed (ee546157 + review fixes: periodicity scan, first-failure contract, splash-before-TPM; Codex 10x); cursor at TODO-12 §6.
- 2026-06-12 ~10:00 SAST: between-runs -- user authorized Monocypher vendoring (D02T03 §5); run-5 queue set (D02T03 §5 -> TODO-12 §6/§7/§8 -> §9/§10 -> closure); headless unit PATH fixed via systemd --setenv (handoff flagged missing ~/.local/bin + nvm node).
- 2026-06-12 ~10:00-11:50 SAST: run 5 -- D02T03 §5 Monocypher 4.0.2 + kernel CSPRNG + NtGetRandom 0x03D8 shipped (99910014) + reviewed (21168666); Codex design+test-coverage+adversarial(multi-round)+consistency+perf+re-adversarial; 2 latent bugs fixed (heap_init truncation, USER_PT_WINDOW reservation+payload check); perf/SMP hardening (single-ratchet NtGetRandom, emergency seed I/O out of lock, atomic g_seeded); 2 systemic HIGH accepted-XREF (TODO-10 §2, TODO-12 §12); cross-TODO unblocks landed (TODO-12, TODO-10/11/06-desktop/01-sdk ownership). Next: 01-boot TODO-12 §6.
- 2026-06-12 ~12:00-15:00 SAST: run 6 (interactive /overnight-runner:start --resume) -- TODO-12 §6 seed-file carryover shipped (08340720) + reviewed (23b3b611); Codex 16x (design, test-coverage, adversarial x2, adversarial-impl x5, re-adversarial x5, consistency, perf): 1C+10H+9M+2L fixed, 4H+2M accepted-XREF, 2 rejected with evidence; landed with it: FAT32 sector-cache coherence (new files read back all-zeros), LFN-aware validated delete, vfs_unlink/vfs_rename_ex parent re-resolution, partition blkdev zero-init (#UD wild call into boot_info), durable vfs_flush (dirty-since-sync gate under new sync_mutex), NtFlushBuffersFile failure propagation, hardware-provenance gate on seed mint/rotation; evidence: 4565+16 tests, smoke PASS, live multi-boot carryover (mint->rotate->accept->anti-replay->clone-reset recovery). Next: TODO-12 §7.
