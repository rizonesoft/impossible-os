---
name: review-todo-section
description: Full review of a TODO section -- adversarial Codex, consistency, performance, domain code quality, feature completeness. Runs after implementation or standalone. No steps skipped.
---

# Review TODO Section

> **External-Reviewer Contract:** This skill dispatches Codex as part of its workflow. Every finding goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

> **NO CODE SHIPS WITHOUT BEING EXAMINED FROM EVERY ANGLE.**
>
> You have been observed cutting corners: skipping Codex dispatches, self-reviewing instead of dispatching, accepting "clean" without running quality gates, folding separate concerns into a single shallow pass. This skill exists because your judgment about which steps to skip has been wrong repeatedly.
>
> Every step runs. Every time. Mechanically. No judgment calls about skipping.
>
> Technical completion is not enough. This review must also catch **false completeness**: code that satisfies the listed checklist items but still leaves the feature obviously incomplete, poorly wired, uncompetitive, or missing a tracked owner for adjacent work.

## Pipeline

### Phase 1: Evidence + Verify

1. **Evidence map** -- for each `[x]` item, prove via Grep/Read: "claim -> file:line". Downgrade on regression. **By default, first dispatch `Agent(subagent_type="review-evidence-mapper", ...)`** (skip only for tiny stamp-only/docs sections) (read-only, Sonnet) to build the breadth of the map -- claim->code, surrounding surface, risk hotspots -- in a separate context. Then do your own >=2 `src`/`include` `Read`/`Grep` calls on its top-listed spots to verify them: those MAIN-SESSION reads are what satisfy the Phase-1 evidence gate (`phase1_evidence_gate.py` counts only this session's reads, NOT a subagent's), and they double as verification of the mapper's claims. The mapper only removes the long-tail reading from this loop; never bypass the gate via `SKIP_PHASE1_BLOCK`.
2. **Scope-gap audit** -- grep for `TODO`, `FIXME`, `HACK`, `STATUS_NOT_IMPLEMENTED`. Standalone stubs: implement. Infrastructure stubs: Accept with XREF.
3. **Test checkpoint verification** -- read the Test checkpoint paragraph. Grep source for each expected serial/klog message.
4. **Build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`. (Full test-suite or smoke runs needed during this review go through `Agent(subagent_type="checks-runner", ...)`; quote the artifact tail yourself.)

### Phase 2: Adversarial Review

5. **MANDATORY Codex adversarial review** -- dispatch to Codex. No exceptions.
   ```bash
   bash scripts/codex-dispatch.sh '[review-kind: adversarial] <todo-path> <prompt>'
   ```
   **Prompt shape:** start with `[review-kind: adversarial]` and include the TODO path (`todo/<domain>/TODO-XX-<slug>.md`) + `§<N>`. The §3 PostToolUse hook (`codex_review_completed.py`) records this dispatch to `.claude/state/last-review-stamps.json` keyed by TODO path. Without the marker AND the path, the §4 commit gate cannot attribute the dispatch and the three-dispatch evidence check (step 16 stamp commit) refuses the commit. Use [`codex-prompt-template.md`](codex-prompt-template.md) (adversarial section); do NOT prepend implementor narrative -- the hook WARNs on first-person preambles per [CONSENSAGENT ACL-2025](https://aclanthology.org/2025.findings-acl.1141/).

   **Mandatory angles:** integer overflow, buffer overread, NULL deref, SMP races, resource leaks, ABI mismatch, bounds on untrusted data. The PostToolUse hook fires `receiving-code-review` reminder; follow it. Triage findings -> fix valid Critical/High/Medium, reject false positives with code evidence, accept out-of-scope with domain-qualified XREF.
6. **Fix adversarial findings** -- fix all valid Critical/High/Medium. **Rebuild via `Agent(subagent_type="checks-runner", ...)` by DEFAULT on every fix round** (build + any test rerun; quote the `build/build.log` tail yourself -- verification-before-completion stays a main-session read). When a build failure produces a large log, dispatch `Agent(subagent_type="diagnostic-digester", <log path>)` first and validate its hypotheses at `file:line` before editing -- do not read the whole log into this context. Repeated symbol lookups across fix rounds go through lsp-bridge (`definition`/`references`), not inline `bash grep`.

### Phase 3: Quality Audit

7. **Domain code quality gates** -- **First, for `src/kernel/` or `src/boot/` sections, dispatch the matching read-only auditor agent as a pre-pass** -- it walks the gates against the diff in a separate context and returns findings; triage them per `superpowers:receiving-code-review`:
   - `src/kernel/`, `include/kernel/` -> `Agent(subagent_type="kernel-quality-auditor", ...)` (Opus; SMP/lock-order/bare-metal lead)
   - `src/boot/` -> `Agent(subagent_type="boot-quality-auditor", ...)` (Sonnet; UEFI/EBS/ABI/POST16 gates)

   **When a fresh same-diff auditor dispatch returned its walk, its findings ARE the mechanical gate pass** -- triage them per `superpowers:receiving-code-review` (verify each at file:line); do NOT also re-read the domain skill file and re-walk all gates in this session. Measured 2026-07-03 (run-20260702-141810.log): all 8 auditor dispatches were followed within seconds by a full main-session `kernel-code-quality/SKILL.md` read + inline re-walk -- pure duplication on a diff that still gets the two step-8 Codex quality dispatches plus adversarial. The main-session full walk is REQUIRED only when the auditor was not dispatched or its dispatch failed:
   - `src/boot/` -> read and walk `boot-code-quality` (all 10 gates)
   - `src/kernel/` -> read and walk `kernel-code-quality` (all 10 gates)
   - `src/desktop/` -> `desktop-code-quality` | `src/shell/` -> `shell-code-quality` | `user/` -> `userland-code-quality` (no auditor agents exist for these three yet -- always walk them yourself)
   Record any gate failures (the auditor's + any you catch during triage) as findings.

8. **MANDATORY Codex quality review -- TWO separate focused dispatches.** No exceptions, no combining. A combined "both in one prompt" dispatch is rejected: Codex's attention budget gets diluted across the angles and depth drops. Each dispatch focuses entirely on its named angle with file:line prompts specific to that angle's class of bugs. **Run the two dispatches in PARALLEL** (not sequentially): they share zero state, target the same diff, and cut total wall-clock review time roughly in half. Use the `superpowers:dispatching-parallel-agents` discipline -- send the two `Bash` calls in a single message with multiple tool_use blocks, then triage findings from each independently per `superpowers:receiving-code-review`. Sequential ordering is acceptable as a fallback when one dispatch is hitting Bash's 10-min wall and you need its output to scope the next prompt; the default is parallel.

   > [!NOTE]
   > **Dead-code Codex retired 2026-04-25.** The dead-code dispatch was removed because its highest-risk failure mode (Codex flags a function that a future TODO section will call, the reviewer removes it, the later section has to recreate it) is not mitigated by the skill's post-hoc guardrails. Static caller-graph analysis cannot distinguish "legitimately orphan" from "reserved for §N+M," and rubber-stamped findings introduce regressions that build green and surface weeks later via SSDT / IDT / driver-vtable paths. Dead-code analysis is now out-of-band advisory work owned by a human sweep, not part of the section-ship gate.

   **8a. Consistency dispatch.** Invoke `codex-consistency-audit` via the plugin:
   ```bash
   bash scripts/codex-dispatch.sh '[review-kind: consistency] <todo-path> <consistency prompt>'
   ```
   **Prompt shape:** start with `[review-kind: consistency]` plus the TODO path + `§<N>`. Use [`codex-prompt-template.md`](codex-prompt-template.md) (consistency section). Angles: struct layout must match byte-for-byte across kernel + bootloader mirrors, constants defined in one place (no silent duplication with drift potential), API contracts (signature + error-code semantics match consumer expectations), ABI schemas (NVRAM variable name + GUID + attrs + size match producer/consumer), SSDT row ↔ function ↔ registration consistency, Win32 vs NT semantics, same narrowing / truncation / padding patterns applied uniformly across cross-file changes.

   **8b. Performance dispatch.** Invoke `codex-perf-review` via the plugin:
   ```bash
   bash scripts/codex-dispatch.sh '[review-kind: perf] <todo-path> <perf prompt>'
   ```
   **Prompt shape:** start with `[review-kind: perf]` plus the TODO path + `§<N>`. Use [`codex-prompt-template.md`](codex-prompt-template.md) (perf section). Angles: allocations in hot paths / ISR contexts, O(n²) or worse on unbounded inputs, spinlock hold times spanning I/O or serial writes, byte-at-a-time operations that should be memcpy'd, branch-misprediction hazards on hot paths, cache-line sharing across per-CPU state, inline-asm barriers placed conservatively vs required.

   Each dispatch fires the PostToolUse `receiving-code-review` reminder; follow it on every finding from every dispatch.

   **State-file mechanism (TODO-08 §5):** after dispatching each of the three reviews (step 5 adversarial + 8a consistency + 8b perf), the §3 PostToolUse hook records the dispatch into `.claude/state/last-review-stamps.json` keyed by TODO path with a per-kind ts_ns. The §4 commit gate refuses the section-commit (the one that adds the `**Verified:**` / `**Quality reviewed:**` stamp) until all three entries are present AND within the last 30 minutes for that TODO path. There is no "two is enough" or "three is overkill" fallback -- the gate enumerates the missing kinds in its BLOCK message. **The marker `[review-kind: <kind>]` and the TODO path MUST appear in every dispatch prompt** or the hook cannot attribute the dispatch and the gate treats it as missing.

   **Rationale for separate dispatches instead of one combined.** The one-dispatch combined prompt was valid until ~2026-04-24; observed failure mode was that Codex would deep-dive whichever angle had the most text in the prompt and skim the others. Focused per-angle dispatches give each one its own attention budget, lower the probability of missed findings, and make `Codex 3x` (adversarial + consistency + perf) the scannable baseline counter in the Quality reviewed stamp.

9. **Industry standards + Win11/Linux parity** -- spec compliance, concrete function/file references. **Dispatch `Agent(subagent_type="parity-research-analyst", ...)` in implemented-code mode BY DEFAULT** (Sonnet; skip only for sections with no user-visible parity surface -- pure tooling/docs) to research Win11/Linux behavior and surface parity + false-completeness gaps on the shipped code; fold its sourced findings into this step and step 10. Advisory only -- you decide what to fix now or file as a concrete owner item.

10. **Feature completeness + adjacent completeness** -- grep for `STATUS_NOT_IMPLEMENTED`, partial implementations, dead API promises, and the "one missing piece away from real" pattern:
    - Did the section technically land, but miss the next obvious adjacent capability a real user or caller would hit?
    - Are exports, registrations, tables, docs, tests, or TODO/XREF ownership still missing?
    - Does Win11 or Linux already cover a nearby case that this section still ignores?
    - If the gap is small and same-subsystem, fix it now. If it needs broader infrastructure, file a concrete owner TODO item now.

11. **Test coverage check (CRITICAL)** -- verify unit tests exist for this section:
    - Grep for the section's functions/features in `src/kernel/test/test_*.c`. If no test file covers the section's code, flag as a finding.
    - Check that the correct `TEST_CAT_*` category is used (not piggy-backed onto an unrelated category).
    - Verify the matching bat exists in the **right subdir** for the test layer (split 2026-04-20):
        - Kernel `TEST_CAT_*` -> `scripts/debug/kernel/run-<category>-tests.bat`
        - User-mode `test_*.exe` -> `scripts/debug/usermode/run-<binary>.bat`
        - Desktop UI -> `scripts/debug/desktop/run-<test>.bat`
      If absent, flag it and add the bat to the right subdir (the per-binary user-mode bats are owned by the user-mode test framework TODO and may legitimately not exist yet -- a `**Note:**` in the Unit Tests section explaining the deferral is the alternative). Per-category bats MUST NOT live at the `scripts/debug/` root; that location is reserved for `run-all-tests.bat`.
    - If tests are missing: either add them during this review (for simple read-only checks) or flag with `**Test gap:** <description> -- needs test_<name>.c` in the stamp.
    - If no kernel-side testable surface exists (pure UEFI bootloader code), verify the TODO section has a `**Note:** No kernel test surface -- validation via serial log on WHPX.` note.

    > **Incident 2026-04-12:** TODO-03 §1-§12 and TODO-17 §1-§3 all shipped without unit tests. The TODO-19 §1 review then found 3 critical FPU bugs that tests would have caught. Tests are not optional -- they catch real bugs.

12. **Self-review** -- regressions, races, edge cases, resource leaks, and completion radar:
    - Correctness: does it work on normal and failure paths?
    - Completeness: what still feels obviously missing?
    - Wiring: what is still not connected end-to-end?
    - Parity: what do Win11/Linux still do better here?
    - Superiority: is there an obvious cleanup or refinement we should have taken?
    - Ownership: if not fixed now, where is the exact checklist item that owns it?

13. **Fix ALL findings** from steps 7-12. Priority: spec violations > incomplete stubs > missing tests > quality gate failures > consistency > parity > perf. Rebuild after each batch.

13.5. **Conditional re-adversarial Codex** -- fires only when the cumulative fix diff from steps 6 + 13 is non-trivial. Trigger if ANY of:
   - Touches locking, atomics, memory barriers, or per-CPU state.
   - Touches ISR / interrupt entry / IST stacks / IRQL handling.
   - Touches lifecycle: alloc / free / refcount / handle close paths.
   - Cumulative diff > 50 lines of changed C/H, OR > 3 functions modified.
   - Introduces or rewrites a state machine, a wait/wake path, or a faultable code region.

   If any trigger fires, dispatch a focused re-adversarial scoped to ONLY the fix diff:
   ```bash
   bash scripts/codex-dispatch.sh '[review-kind: re-adversarial] <todo-path> <re-adversarial prompt>'
   ```
   **Prompt scope:** the diff between the pre-fix and post-fix tree (`git diff <pre-fix-sha>..HEAD -- <files>`). Angles: regressions introduced by the fixes themselves, new races opened by lock-order changes, new NULL paths from added error handling, new resource leaks from added early returns, new ABI drift from struct/enum touches.
   The PostToolUse hook fires `receiving-code-review` reminder; follow it. Findings -> verify, fix, build. Track in stamp as `Codex Nx` (incrementing the dispatch count by 1; add `re-adversarial` to the `<kinds>` list).

   **Iteration: keep dispatching re-adversarial until convergence (no new findings) OR a hard cap of 8 rounds.** Updated 2026-04-29 from the previous "3 rounds" cap after the §11 UKI review proved that genuine H findings landed in rounds 4 and 5 -- a low cap caused the agent to stop while load-bearing security gaps were still open. Convergence rule:
   - **A round closes the loop** when Codex returns zero findings OR every finding the round produced is rejected with code evidence (false positive). State this explicitly in chat: "round N: zero findings, loop closed" OR "round N: 2 findings rejected (file:line evidence: <quote>); loop closed."
   - **A round continues the loop** when ANY valid Critical/High/Medium finding is fixed in that round. Every fix MUST be verified at file:line before the next round dispatches.
   - **The hard cap of 8 is a circuit-breaker, not a soft target.** If round 8 still produces valid H findings, STOP, file the remaining gaps as concrete `[ ]` checklist items in the section under Branch B (sub-items in this section) or Branch C/D (new section / new TODO file) per scope-gap protocol, then ship. The pattern of "reach the cap with open findings" is itself a signal that scope is wrong; re-evaluate before continuing.
   - **Defer-to-tracked-followup is NEVER chat-only.** "I'll defer this" without a concrete `[ ]` item in a target TODO is a process violation -- the Accepted-XREF concreteness check (step 15) and the filed-in-owner check (step 18) exist precisely to catch this.

   If no trigger fires, skip and note in the stamp: include `(<reason>)` after the count, e.g. `Codex 3x (adversarial, consistency, perf)` with a brief Notes-block bullet `re-adversarial skipped: docs+stamp-only fixes`.

### Phase 4: Stamp + Commit

14. **Reconcile tables** -- Implementation Order row, OS Comparison row. If the Implementation Order table has an `[x]` entry that does NOT have a corresponding row in the OS Comparison table, ADD the missing row (a missing row is as much drift as a stale one). Check that the post-OS-Comparison summary sentences cover the sections just shipped; if they cap out at an earlier §, extend them.

15. **Accepted-XREF concreteness check (MANDATORY)** -- before writing any "Accepted" line in the stamp, prove every XREF target points at a **concrete, actionable `[ ]` checklist item** that would close the gap when marked `[x]`. A prose mention, an enum definition, or a section title alone is NOT a concrete item.

    For each finding you intend to write as "Accepted":
    - Open the XREF'd target file and section.
    - Search for a checklist item whose body explicitly addresses this finding (names the gap, cites the source file/function to fix, describes the work in operational terms).
    - **If found:** quote the item by name in the stamp (e.g., `XREF: TODO-13 §4 (item: "Migrate HKEY to OB handle table" at line 248)`).
    - **If not found:** create the concrete item NOW:
        - Identify the right owner section. If the natural owner is registry-specific but the gap is kernel-wide, generalize the helper name and place it in a kernel-wide location.
        - Write a checklist item that names the source file/function to retrofit, the helper to add (with signature), and the validation behavior. Cite this section's source file and function as a consumer.
        - If no owner section exists and none fits, follow scope-gap protocol Branch C/D (create new section or new TODO file) BEFORE writing the stamp.
        - Update the stamp XREF to reference the concrete item by name/line, not just the section.

    **Examples of dead-end XREFs to reject:**
    - `XREF: TODO-13 §4` (just the section, no item) -- WRONG
    - `XREF: TODO-13 §4 (KEY_INFORMATION_CLASS completeness)` (paraphrases an enum, not a checklist item) -- WRONG
    - `XREF: TODO-11 §5 (SeAccessCheck)` (section exists, no item for THIS gap) -- WRONG

    **Examples of concrete XREFs to accept:**
    - `XREF: TODO-13 §4 (item: "Migrate HKEY to OB handle table" at line 248 -- registers ObpKeyType, retrofits §14/§15 handlers)` -- OK
    - `XREF: TODO-13 §4 (kernel-wide nt_decode_unicode_string helper; retrofit list explicitly names this section's oa_name, NtCreateSymbolicLinkObject_handler)` -- OK

    **Why this step exists:** without it, "Accepted with XREF" becomes a paper trail that someone later has to chase, find nothing actionable, and re-do the analysis. Every Accepted finding must be one `[x]` away from being fully closed.

15.5. **Verification before stamp (MANDATORY)** -- invoke `Skill(superpowers:verification-before-completion, ...)` to enforce the evidence-before-assertion contract. The stamp's `Verified:` line is a literal claim that build / tests / lint passed; this skill makes you re-run the relevant commands and quote the actual output BEFORE writing the stamp. Closes the `feedback_never_skip_review` failure mode where claims are made without re-running. Required output to capture: `tail -1 build/build.log` (must show `=== BUILD OK ===`), the test runner's pass count (e.g. `bash scripts/test-tooling.sh` final tally), and `bash scripts/lint.sh` exit code. Skip ONLY when the section is docs-only and there's no testable surface; document the skip in the stamp's `scope:` field.

16. **Stamps** -- compact pipe-separated fields, one line per stamp, no blank lines between:

    ```
    > **Verified:** YYYY-MM-DD | commit `<hash>` | N/M items | build OK[ | <evidence token>]
    > **Accepted:** [<sev>] <one-line finding> [(reason: <short>)] -> XREF: NN-domain/TODO-XX §N (item: "..." at line N)
    > **Deferred:** [<sev>] <one-line finding> [(reason: <short>)] -> XREF: NN-domain/TODO-XX §N (item: "..." at line N)
    > **Quality reviewed:** YYYY-MM-DD | Codex Nx (<kinds>) | <H>H+<M>M+<L>L fixed, <D> open | scope: <skill or "N/A (reason)">
    ```

    Field rules:
    - `N/M items` -- `[x]` count vs total `[ ]+[/]+[x]` in the section (exclude the `Commit:` line).
    - `build OK` | `build FAIL` -- single token. Do NOT re-paste `=== BUILD OK ===`.
    - **Evidence token vocabulary (pick one; keep scannable):** `smoke PASS (<platform> <time>)` for boot-path work; `tests N/M PASS` when a dedicated suite ran; `<N> sentinels` / `<N> fields` / `<N> rows` for structural counts; `manual` when validation is manual walkthrough. Free-form is still allowed; prefer the vocabulary. Skip if nothing surprising.
    - Codex line: `Nx` is dispatch count (minimum 3 per current rules: adversarial + consistency + perf; bumps to 4 when step 13.5 re-adversarial fires). `<kinds>` names dispatches (`adversarial`, `consistency`, `perf`; the legacy combined `quality` kind is no longer accepted -- split it into explicit entries; the legacy `dead-code` kind was retired 2026-04-25 and MUST NOT appear in new stamps). Add `re-adversarial` when step 13.5 fires; add `design` for upstream design dispatches that landed in the same effort. Findings as `<H>H+<M>M+<L>L fixed[, <D> open][, <N>M accepted-XREF]`; drop zero terms. **Keep the count terse: `6H+5M+7L fixed, 2M accepted-XREF` is the right shape; `6H+5M+7L fixed (4H+3M+3L design adopted pre-code, 1H+1M+1L implement adversarial, ...)` is OVER-DETAILED -- per-dispatch breakdowns belong in the commit message, not in the stamp.** `scope:` names the domain code-quality skill applied OR `N/A (<reason>)` like `N/A (docs-only)`. Do NOT write the full "No domain code-quality skill applies (...)" sentence.
    - **`Accepted:` vs `Deferred:` -- pick the right label; the semantic split matters for triage.** Both are hook-enforced identically.
      - **`Accepted:`** -- finding valid but **out-of-scope for this section**; ownership is elsewhere. XREF points to a concrete item in ANOTHER TODO section or file. Grep `Accepted:` when auditing ownership-transfer risk.
      - **`Deferred:`** -- finding valid, **in-scope** for this TODO, but bigger than this commit. XREF points to a later item in THIS section or TODO file. Grep `Deferred:` when auditing "we promised to come back to this."
    - **Severity tag** `[Critical]` / `[H]` / `[M]` / `[L]` is REQUIRED on every Accepted and Deferred line. Matches Codex finding severity. Enables `grep -r "Accepted:\s*\[H\]" todo/` for fast scope-risk triage.
    - **Optional `(reason: <short>)`** -- include when the defer-rationale is not obvious from the finding. Common reasons: `scope` (belongs to another owner), `infra` (needs bigger work unit), `not-functional-today` (HW/deployment guarantees mean not racing a real bug). Keep under ~10 words. Omit when the finding text alone explains the defer.
    - Gets its OWN blockquote line between Verified and Quality reviewed. One XREF per concern; repeat the line for multiple. Domain-qualified (e.g. `02-kernel-core/TODO-17`, never bare `TODO-17`). `(item: "<name>" at line N)` parenthetical is MANDATORY (hook-enforced, see step 15 grammar). If there are NO accepted/deferred items, OMIT both lines entirely -- do NOT write `Accepted: none`.
    - **Escape hatch (rare):** if per-finding prose is genuinely load-bearing (cross-review context the commit message can't carry), append a `<details>` block after the stamps:
      ```
      > <details><summary>Finding detail</summary>
      >
      > - H1 <one line> -> fixed at file:line
      > - M1 <one line> -> deferred (Deferred above)
      > </details>
      ```
      Default is NO `<details>` block. Commit messages and `[x]` marks carry most audit weight; counts + XREFs carry the rest.
    - **Do NOT re-emit a `> **Test runner:**` line** -- the pre-stamp block written by `implement-todo-section` step 8 is the single source of truth. If the test count or bat file changed during review, edit the pre-stamp block in place instead of adding a second line.
    - **Verify the `> **Notes:**` block is present + is canonical-shape** (MANDATORY for sections marked `[x]` or `[/]`). Canonical order of the pre-stamp + stamp region: Test checkpoint paragraph -> blank -> `> **Test runner:**` -> blank -> `> **Notes:**` -> blank -> `> **Verified:**` -> `> **Accepted:**` (if any) -> `> **Deferred:**` (if any) -> `> **Quality reviewed:**`. If the Notes block is missing, ADD it now using the grammar in `implement-todo-section` step 10 (3-6 bullets: what shipped, how it runs/integrates, downstream effects, canonical doc pointer, scope boundary -- ONE bullet each, ONE line each, NO sub-bullets, NO per-finding adoption sub-blocks). **If the Notes block is over-stuffed** (more than 6 bullets, or any sub-bullets, or per-finding "Design review adoptions" / "Implementation adversarial adoptions" / "Latent bug fixed" / etc. sub-blocks): **trim it back to canonical shape during this review pass.** Adoption details belong in commit messages, not Notes. A PreToolUse hook in `.claude/settings.json` (the Notes-block bloat hook) BLOCKS edits that introduce > 6 bullets or sub-bullets; if the hook fires during the review, that's the signal to delete content, not to opt out via SKIP_REVIEW_HOOK. The stamps are the machine-readable audit trail; commit messages are the per-finding evidence trail; Notes is the 30-second human scan summary.

17. **Commit and push** -- `"review: <TODO> §N -- <summary>"`. If step 15 created or modified items in other TODO files, stage and commit those in the SAME commit as the stamp.

## Rules

- **THREE Codex dispatches per review.** Step 5 (adversarial), step 8a (consistency), step 8b (performance). All three mandatory, no combining, no skipping. A single combined consistency+perf dispatch IS rejected at stamp time -- the `Quality reviewed: Codex Nx` counter must show at least 3x (adversarial + consistency + perf) for a review to be accepted. The legacy dead-code dispatch was retired 2026-04-25; dead-code analysis is now out-of-band advisory work, not a gate.
- **Domain code quality skill walked explicitly** in step 7. Not just the hook -- read the skill and check every gate.
- **Test coverage verified** in step 11. Missing tests are a finding, not acceptable.
- **False completeness is a finding.** Checklist satisfaction without credible feature completeness or tracked ownership does not pass review.
- `superpowers:receiving-code-review` on every Codex finding (PostToolUse hook fires the reminder; follow it -- no in-step prose duplication needed).
- All stamps use domain-qualified XREFs.
- All stamps include test runner bat file and expected results.
- No blank line between stamps.

## TODO-08 §10 step-state telemetry

The skill-step-observer hook records each step on its real tool call; the skill-step-block hook BLOCKs commit if any required terminal step's evidence is missing. There is no "I did it inline" shortcut -- the hook does not see narration. The hook fires on `Bash(git commit:*)` and `Skill(review-todo-section)`. Required terminal steps for this skill are listed in `.claude/hooks/skill_step_map.py`. Opt-out (legitimate revert / stamp-only flows): `SKIP_SKILL_STEP_BLOCK=1 SKIP_SKILL_STEP_BLOCK_REASON="<text >= 12 chars>"`.
