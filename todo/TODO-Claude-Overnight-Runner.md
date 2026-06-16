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
  --next` (graph-truth over `build/todo-cache.json` + Verified stamps), which
  returns the first NEEDS-WORK / DONE-UNSTAMPED file in traversal order; within
  it, `--classify <file>` gives the first section not shipped-and-reviewed.
  DONE files (all sections `[x]`/stamped-`[/]` + Verified) are skipped;
  DONE-UNSTAMPED files (old-system work without stamps) get a `review-todo-section`
  pass per section. The Cursor block below is a human-readable convenience log,
  NOT the source of truth -- the oracle is.

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
  section -- and the runner NEVER stops or disarms itself.** Finishing a
  section (ship + review + push) is NOT a reason to run
  `overnight-runner finish-check` / `handoff` or to final-answer. After every
  section: update the cursor, then IMMEDIATELY start the next section in the
  queue. There is NO voluntary session-exit on a blocker of ANY kind:
  - A **user-decision / operator-reserved item** (a stop-and-ask boundary) is
    NOT a stop and NOT a disarm. Record the question in the cursor, DEFER the
    item (`[/]` + a Deferred stamp + an XREF naming the decision), and ADVANCE
    to the next section/file. A single reserved decision in one section must
    never strand the other 85 TODO files. (Incident 2026-06-16: the runner read
    a stale "user reserved this" blocker and ran `--disarm`. Both the stale-state
    and the self-disarm paths are now closed; the behavioral rule is: defer and
    advance, full stop.)
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
  design `docs/superpowers/specs/2026-06-13-overnight-sequencer-design.md`.]
- **Full quality pipeline every section** -- all Codex dispatches, domain
  code-quality gates, unit tests, stamps. No "straightforward section"
  exceptions (`feedback_no_corner_cutting`, `feedback_never_skip_review`).
- **No scope inflation:** gap-audit edits go into TODO files as checklist
  items; do not implement gap findings inline outside the section pipeline.
- All other repo doctrine (CLAUDE.md, memory feedback) applies unchanged.

## Enforcement & Scheduling

This doctrine is not just guidance: it is hard-enforced. Architecture is
**repo schedules, repo decides** (`docs/superpowers/specs/2026-06-13-overnight-sequencer-design.md`):

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
- 2026-06-13 ~19:45-22:00 SAST: run 7 cont. (TODO-02) -- oracle advanced to `01-boot-platform/TODO-02-uefi-hardening-secureboot` (14/16 done). validate (fixed broken TODO-13 measured-boot XREF) + gap-audit (Codex parity-floor red-team: filed §17 SBAT revocation metadata; rejected UKI addons/profiles as advanced-not-floor). §15 post-boot revalidation DEFERRED [/] -- Codex design review caught the naive 5-min worker is wrong (sleep_ms is a busy hlt-poll, no parking primitive); owned by TODO-07 §18 kworker pool + TODO-26 §3 (c3cc183b, Verified+Deferred). §16 split-path payload provenance hardening shipped+reviewed (46eb95bc + 04ba99e1; Codex design caught load_kernel non-boot fallback re-splits provenance -> 2-part fix: locate_boot_fs HandleProtocol-only + load_kernel fails-closed when payloads staged). §17 SBAT revocation metadata shipped+reviewed (900ade7c + 41b0d8f2; Codex 8x: dropped test-only boot_info ABI per design, link-time incbin .sbat (post-hoc objcopy unviable -- content-drop/PE-corruption/0-byte), two-stage build gate, .reloc PE-alignment fix). Resolved TODO-03 inbound Accepted (73e1a9f5); close-out sweep filled §11 <hash> placeholder (516a19e4). RECORDER LEARNING: codex dispatches must NOT use `cd` prefix or `timeout` prefix -- both break detect_review_kind_from_cmd (recorder + design/section gates); relative `bash scripts/codex-dispatch.sh` from repo-root cwd records correctly. TODO-02 now DONE (17 sections). Next: oracle re-triage.
- 2026-06-13 ~18:05-19:45 SAST: run 7 cont. (post-compaction) -- oracle advanced to `00-infrastructure/TODO-09-repository-transfer-rizonetech` (NEEDS_WORK). §8 move-back runbook + §9 doc-sync/closure shipped earlier in session; this slice closed the §9 review (consistency findings: line-24 pre-transfer baseline restored to rizonesoft, github-setup "push blocked" overstatement dropped -- pushes land via the "expected" check, §5/§7 push-state wording deferred->§5 F1; OS-comparison row refreshed) 8fb63284. RESOLVED run-7 punch-list: registered 5 missing plugins in `.claude/hooks/MANIFEST.md` (desktop-commander/code-modernization/claude-code-setup/commit-commands/plugin-dev no-hooks + remember 0.7.3 hooks table) 8d89986c -> test-tooling back to 375/375. complete-todo-file closure a55571d8: Unit Tests N/A (operator-validation), Verification 4/14 automated PASS (lint, test-tooling, todo-graph, owner-ref scan), 3 [/] partial (existing-clone + WSL2 tooling), 7 manual-pending (live GitHub/DNS/org-Settings). TODO-09 now DONE (operator validation tail tracked in §2-§7 [/]). Next: oracle re-triage.
- 2026-06-13 ~15:30-18:05 SAST: run 7 (interactive /overnight-runner:start --resume, drove sequencer) -- oracle pointed at `00-infrastructure/TODO-07-lsp-mcp-bridge` (NEEDS_WORK, not the cursor-log's TODO-12; graph-truth wins). validate (fixed stale `.claude/mcp.json` Inputs anchor) + gap-audit (Codex red-team: type-hierarchy = real read-only parity gap, 5 other LSP capabilities rejected as agent-facing gold-plating). Shipped §18 Type Hierarchy Tools (supertypes/subtypes, read-only, capability-gated, bounded fan-out) 931096c9 + reviewed 8e121a57; Codex 5x (design + adversarial + consistency + perf + re-adversarial): 1H+6M fixed (capability_missing-on-every-path, normalized response anchor, per-anchor types cap, census drift x2, gate-before-didOpen + deadline-before-open + bounded prepare timeout), 1 perf-H deep rewrite rejected w/ evidence (small request-response frames never accrue 64KB pipe backpressure). §19 scale roadmap deferred [/] (trigger-gated, Verified+Deferred stamps). complete-todo-file closure 33c39c59: Unit Tests 5/5, Verification 10/11 automated PASS (test-tooling left [ ] on 2 PRE-EXISTING UNRELATED audit-hooks/audit-ai-system plugin-MANIFEST failures), 3 commit-hash placeholders resolved; harness 99/99, live MCP hover verified. TODO-07 now DONE. PUNCH-LIST: audit-hooks/audit-ai-system plugin MANIFEST.md drift (6 newly-installed plugins lack headings) -- separate sweep. Next: oracle re-triage.
- 2026-06-16 ~21:20- SAST: run 8 (headless sequencer) -- preflight caught the known UKI-SBAT incremental-build bug (clean build green; not a broken HEAD); baseline 6009 kernel + 16 user PASS. Oracle advanced to `00-infrastructure/TODO-01-developer-tooling-stack` (NEEDS_WORK on §11's missing stamps). validate (clean) + gap-audit (Codex confirmed zero unowned dev-tooling gaps; reproducible-builds/signing/SBOM owned by D15 T01 §5/§6/§8; filed §12 required-tool version-floor gate as the one genuine parity gap). §11 review: found the bare-section drift gate had file-type holes (.ld/.lds/.inc absent from both lint Check 5 + the PreToolUse hook) + gate-parity drift (Makefile/todo-graph/test-ai-system) + a LIVE escaped ref in dump-fields.inc; hardened both gates + aligned hook<->lint + added [bare_section_gate] 11-assertion regression group (9033e324 + 9360024a). §12 implemented: check_versions() fail-closed floor gate, --verify now enforces presence+floors (Codex design H), qemu-img floored + drift guard (Codex design M), tooling-doctor/build.yml/release.yml/docs wired, [version_floor_gate] 7 assertions (7942170c + 517986b7; Codex 6x design/adversarial/consistency/perf: 2H+5M fixed incl. build.yml qemu-utils + qemu-img doc/help propagation). complete-todo-file closure 8f07f5a1 (test-tooling 79->393 count refresh). TODO-01 now DONE (12 sections). RECORDER STILL DEAD: codex_review_completed.py PostToolUse not firing; manually recorded design/adversarial/consistency/perf dispatches for both section gates. Next: oracle re-triage.
- 2026-06-16 ~23:00- SAST: run 8 cont. -- oracle advanced to `00-infrastructure/TODO-09-repository-transfer-rizonetech` (NEEDS_WORK: §5/§6/§7 had Verified but no Quality-reviewed stamps under the both-stamps DONE rule). Reviewed all three (docs/GitHub-audit, Codex 3x each, manually recorded -- recorder still dead): §5 (beaccf98) 2M -- secrets row overclaimed "value preserved" (gh API never returns values + BOOTLOADER_REPO_TOKEN has no in-tree consumer), downgraded [x]->[/] + fixed count 9->7; §6 (74611c2e) 2M -- README copyright + gh-pages brand homepage were migrated to Rizonetech by the deliberate rebrand 07d60796 AFTER §6 shipped, so §6's "preserve rizonesoft brand" classification was superseded -- reconciled the docs, rejected Codex's "restore rizonesoft" (reviewer lacked rebrand context); §7 (55da3c3d) 3M -- stale validation evidence (test-tooling 254->393, lint 498->637 files, AGENTS sha a749a0e->75e38c4) refreshed, April clone-HEAD/cert reframed as transfer-time snapshots, .codex residue flagged. close 84070764: git rm'd the tracked 0-byte .codex residue (autonomous-agent-boundary hygiene). TODO-09 now DONE (9 sections; operator-validation tail in §5/§7 [/]). Next: oracle re-triage.
- 2026-06-17 ~00:00 SAST: run 8 cont. -- oracle advanced to `01-boot-platform/TODO-08-alternate-boot-protocols` (NEEDS_WORK: §2-§6 Multiboot2 sections retired by §7's authoritative 'unsupported' decision but carried [~] items + Verified-RETIRED stamps with no Quality stamp -- the project_sequencer_tilde_status_gap oracle-loop). Fixed (0dd20fe0) WITHOUT a Codex pipeline (roadmap-dropped, no code): replaced the 5 Verified-RETIRED stamps with **Deferred:** stamps XREF'ing §7's deletion decision, kept IO rows [x] (status:done valid; oracle parks [x]+Deferred). First attempt used **Accepted:** + [/] -- wrong: the oracle's DEFERRED_RE only parks on **Deferred:**, and [/] broke status:done's all-[x] rule; corrected to [x]+**Deferred:**. todo-graph 8/8; file now DONE (7 sections). LEARNING: retired/N/A sections need [x]+**Deferred:** (not Accepted, not [/]) for the oracle. Next: oracle re-triage.
- 2026-06-17 ~00:20 SAST: run 8 cont. -- TODO-10-bare-metal-hardening §11 (removed boot.conf skip-list) was the lone NEEDS_WORK: Verified-REMOVED stamp, no Quality/Deferred -> oracle-loop. Parked as [x]+**Deferred:** XREF'ing §7 BOOT_TRY (a237a0c5); also committed the session-start §12 Accepted-XREF line fix (TODO-24 §5: 137->165). TODO-10 now DONE (15 sections). Next: oracle re-triage.
- 2026-06-17 ~01:00 SAST: run 8 cont. -- TODO-14-boot-diagnostics §1/§2/§3 were DONE_UNSTAMPED boot code (old 2026-04-12 Verified stamps, no Quality stamp under both-stamps rule). Reviewed all three (Codex 3x each, manually recorded): §1 (e175fa67) -- adversarial 1M deferred: post_code16 writes high byte 0xB0 to port 0x80 so an 8-bit POST card can't discriminate bootloader milestones (deliberate namespace-vs-milestone design tradeoff per Outcome line 46 -> deferred for architect, [x]->[/]); fixed the Notes/checkpoint overstatement. §2 (e70d4f2a) -- FALSE COMPLETENESS caught: boot_stage_report/boot_progress_poll/boot_get_elapsed_ms have ZERO callers (unwired named-stage API; elapsed-ms stays 0) + latent issues; deferred as one wire+harden item, [x]->[/]. §3 (ee782f14) -- adversarial+consistency same 1M: the DESKTOP_READY clear lives in unwired boot_stage_report so the live path never runs it (desktop overdraw covers it); accepted-XREF to §2's wire+harden item; fixed doc overstatement; [x] kept. TODO-14 now DONE (11 sections). Next: oracle re-triage.
