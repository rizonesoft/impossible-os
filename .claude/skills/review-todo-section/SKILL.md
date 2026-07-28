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

1. **Evidence map -- DETERMINISTIC FIRST.** For each `[x]` item, prove via Grep/Read: "claim -> file:line". Downgrade on regression. **Run `python3 scripts/overnight/diff-facts.py` FIRST** (worktree-aware; or `--range <section-commit-range>` for a committed diff): it derives the changed defs/symbols/callers/header-includers/tests/registration touches, a concurrency/safety inventory (locks, atomics, IRQ state, allocations, frees, refcounts, teardown, user-pointer boundaries, ABI), and cppcheck static findings -- and writes a content-bound receipt. Your reads and the Codex prompts target that bounded set (widen when it reports `global_state_escalation`). The gates themselves still cover the whole diff. **A fresh diff-facts receipt SATISFIES the `review_dispatch_gate` mapper requirement** (it IS the mapper's deterministic evidence-gathering). Route the remaining, JUDGMENT part by diff size:
- **Tiny diff** (`diff-facts` cone_size <= ~5 files, no `global_state_escalation`): Opus reads the changed files DIRECTLY. No mapper dispatch. Your reads satisfy the Phase-1 gate.
- **Large diff**: the diff-facts receipt already clears the mapper gate; dispatch `Agent(subagent_type="review-evidence-mapper", ...)` (read-only, Sonnet) ONLY if diff-facts left genuine ambiguity a breadth pass would resolve, then verify the 1-2 highest-risk slices. Those MAIN-SESSION slice reads satisfy the Phase-1 gate (`phase1_evidence_gate.py` counts only this session's reads).
- **Kernel / security / ABI diff** (any size): diff-facts for the mechanical inventory PLUS >=2 independent main-session spot-checks on the riskiest slices -- the extra net is warranted for the repo's most expensive bug class. (The kernel-quality-auditor / boot-quality-auditor JUDGMENT nets in step 7 stay mandatory -- diff-facts never replaces those.)
Never bypass the gate via `SKIP_PHASE1_BLOCK`. Verification reads are SLICES (`Read(offset, limit)` at the cone's / mapper's file:line targets), not whole-file re-reads.
2. **Scope-gap audit** -- grep for `TODO`, `FIXME`, `HACK`, `STATUS_NOT_IMPLEMENTED`. Standalone stubs: implement. Infrastructure stubs: Accept with XREF.
3. **Test checkpoint verification** -- read the Test checkpoint paragraph. Grep source for each expected serial/klog message.
4. **Build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`. (Full test-suite or smoke runs needed during this review go through `bash scripts/overnight/run-artifact.sh <label> -- <cmd>` -- deterministic envelope, no model; quote the artifact tail yourself. Dispatch `diagnostic-digester` only on a FAIL envelope.)

### Phase 2: Adversarial Review

5. **MANDATORY Codex adversarial review** -- dispatch to Codex. No exceptions.
   ```bash
   bash scripts/codex-dispatch.sh '[review-kind: adversarial] <todo-path> <prompt>'
   ```
   **Prompt shape:** start with `[review-kind: adversarial]` and include the TODO path (`todo/<domain>/TODO-XX-<slug>.md`) + `§<N>`. The §3 PostToolUse hook (`codex_review_completed.py`) records this dispatch to `.claude/state/last-review-stamps.json` keyed by TODO path. Without the marker AND the path, the §4 commit gate cannot attribute the dispatch and the three-dispatch evidence check (step 16 stamp commit) refuses the commit. Use [`codex-prompt-template.md`](codex-prompt-template.md) (adversarial section); do NOT prepend implementor narrative -- the hook WARNs on first-person preambles per [CONSENSAGENT ACL-2025](https://aclanthology.org/2025.findings-acl.1141/).

   **Mandatory angles:** integer overflow, buffer overread, NULL deref, SMP races, resource leaks, ABI mismatch, bounds on untrusted data. The PostToolUse hook fires the `receiving-code-review` reminder; this dispatch is a wave of one, so receive it once (cadence rules in step 8). Triage findings -> fix valid Critical/High/Medium, reject false positives with code evidence, accept out-of-scope with domain-qualified XREF.
6. **Fix adversarial findings** -- fix all valid Critical/High/Medium. **Rebuild via `bash scripts/overnight/run-artifact.sh fixloop -- bash scripts/build.sh` on every fix round** (deterministic envelope; a green run costs zero model tokens; quote the `build/build.log` tail yourself -- verification-before-completion stays a main-session read). When a build failure produces a large log, dispatch `Agent(subagent_type="diagnostic-digester", <log path>)` -- failures ONLY, never green runs -- and validate its hypotheses at `file:line` before editing. **After a SIGNIFICANT fix round (new functions, changed signatures, or fixes spanning 3+ files), dispatch `Agent(subagent_type="test-coverage-mapper", ...)` to re-map the test surface** -- the fixes may have opened coverage gaps the original tests never targeted; fold missing-coverage items into this round's test additions. Repeated symbol lookups across fix rounds go through lsp-bridge (`definition`/`references`), not inline `bash grep`.

   **PRE-DISPATCH SELF-DIFF GATE (mandatory before EVERY Codex re-dispatch in this loop -- the canonical rule; the other codex-* fix loops reference it).** Before re-dispatching, read the fix diff (`git diff` of just this round's edits) against this round's AND every prior round's findings, and self-check four shapes: (1) **Scope** -- addresses ONLY this round's findings; (2) **Ordering** -- no operation moved before its precondition; (3) **Predicate shape** -- `x & FLAG` vs `x == FLAG`, sentinel/boundary values handled as the original path did; (4) **Regression** -- does not re-open any PRIOR finding. Only re-dispatch when the self-diff is clean; a localized non-structural fix that passes: self-verify and proceed WITHOUT a full round. Incidents behind each check: [references/fix-loop.md](references/fix-loop.md).

   **Test-only / cosmetic fix deltas -- lighter confirming path (B2).** A fix diff CONFINED to test files (`test_*.c`, anything under a `/test/`|`/tests/` dir) or to cosmetic edits (comments, whitespace, table alignment, stamp/Notes prose, numeric-section-ref rewrites) does not change shipped behavior: self-verify (assertion still checks the right behavior AND the owning `run-artifact.sh` suite is green) and proceed. At most ONE scoped confirming re-adversarial, and only when the delta changes WHAT is asserted. When a finding asks you to harden a test, never introduce a fragile allocator-/layout-dependent bound -- use a structural invariant. Both rules and the 7-review spiral that produced them: [references/fix-loop.md](references/fix-loop.md).

   **Convergence gate (P2.1/P2.2) -- consult BEFORE every re-dispatch of a kind.** Run `python3 .claude/hooks/review_convergence.py should-redispatch '<todo-path>#<section>' <K>`: exit 1 = **CONVERGED** -> SKIP that kind this round; exit 0 = redispatch. After K's round resolves, `python3 .claude/hooks/review_convergence.py record '<todo-path>#<section>' <K>`. Per-kind fingerprint scope and the fail-open guarantee: [references/fix-loop.md](references/fix-loop.md). This is the PRIMARY churn mechanism; the round counter below is the stall backstop.

   **Round counter + standing evidence map (P2.3).** After EVERY Codex re-dispatch, bump the durable counter: `python3 .claude/hooks/review_round_guard.py --bump '<todo-path>#<section>' --progress <new|none>`. Exit 2 = CAPPED -> STOP the loop, spin unresolved findings to a concrete follow-up `[ ]` + XREF, escalate. **For rounds >= 4 (REQUIRED, not advisory -- check `--status`), verify findings at file:line through ONE `Agent(subagent_type="review-evidence-mapper", ...)` dispatch, NOT inline re-reads of the same hot files** (cache-served free across rounds when src/todo content is unchanged).

### Phase 3: Quality Audit

7. **Domain code quality gates** -- **First, for `src/kernel/` or `src/boot/` sections, dispatch the matching read-only auditor agent as a pre-pass** -- it walks the gates against the diff in a separate context and returns findings; triage them per `superpowers:receiving-code-review`:
   - `src/kernel/`, `include/kernel/` -> `Agent(subagent_type="kernel-quality-auditor", ...)` (Opus; SMP/lock-order/bare-metal lead). **For a diff touching locks, atomics, ISR paths, refcounts, or teardown, dispatch `Agent(subagent_type="concurrency-evidence-mapper", ...)` (Sonnet) IN PARALLEL with it** -- the mapper inventories locks/atomics/IRQ-context/ownership/refcounts/teardown at file:line WITHOUT verdicts, so the Opus auditor (and your triage) starts from a small evidence set instead of raw files. The auditor remains the sole authority on safety; the mapper only shrinks its reading. **D3 -- both in ONE message, and verify BOTH actually fired.** When you dispatch these in parallel, issue BOTH `Agent` calls in a SINGLE message (multiple tool_use blocks, per `superpowers:dispatching-parallel-agents`); NEVER narrate "kick off X and Y in parallel" and then issue only one. Before you triage, confirm you are holding BOTH agent results -- a narrated-but-unfired mapper is a silent gap (measured: on the SMP-heavy §17 the concurrency-evidence-mapper was announced but never dispatched, and the section then shipped a lock-semantics regression a standing lock/atomic inventory would have pre-flagged). If you catch yourself past the dispatch line missing an agent result you announced, dispatch it now before proceeding.
   - `src/boot/` -> `Agent(subagent_type="boot-quality-auditor", ...)` (Sonnet; UEFI/EBS/ABI/POST16 gates)

   **When a fresh same-diff auditor dispatch returned its walk, its findings ARE the mechanical gate pass** -- triage them per `superpowers:receiving-code-review` (verify each at file:line); do NOT also re-read the domain skill file and re-walk all gates in this session. Measured 2026-07-03 (run-20260702-141810.log): all 8 auditor dispatches were followed within seconds by a full main-session `kernel-code-quality/SKILL.md` read + inline re-walk -- pure duplication on a diff that still gets the two step-8 Codex quality dispatches plus adversarial. The main-session full walk is REQUIRED only when the auditor was not dispatched or its dispatch failed:
   - `src/boot/` -> read and walk `boot-code-quality` (all 10 gates)
   - `src/kernel/` -> read and walk `kernel-code-quality` (all 10 gates)
   - `src/desktop/` -> `desktop-code-quality` | `src/shell/` -> `shell-code-quality` | `user/` -> `userland-code-quality` (no auditor agents exist for these three yet -- always walk them yourself)
   Record any gate failures (the auditor's + any you catch during triage) as findings.

8. **MANDATORY Codex quality review -- TWO separate focused dispatches.** No exceptions, no combining. A combined "both in one prompt" dispatch is rejected: Codex's attention budget gets diluted across the angles and depth drops. Each dispatch focuses entirely on its named angle with file:line prompts specific to that angle's class of bugs. **Run the two dispatches in PARALLEL** (not sequentially): they share zero state, target the same diff, and cut total wall-clock review time roughly in half. Use the `superpowers:dispatching-parallel-agents` discipline -- send the two `Bash` calls in a single message with multiple tool_use blocks, then apply `superpowers:receiving-code-review` ONCE over BOTH legs' findings together (see "Reception cadence" below) rather than once per leg. Sequential ordering is acceptable as a fallback when one dispatch is hitting Bash's 10-min wall and you need its output to scope the next prompt; the default is parallel. **In an unattended sequencer run, prefer the broker shape** -- one `bash scripts/overnight/review-broker-codex-dispatch.sh '[review-kind: adversarial] <todo-path> <body>'` call per kind (the wrapper basename is hook-recognized, so per-kind receipts attribute exactly like a foreground dispatch) + ONE structural wait over the returned logFiles + ONE `review-envelope.py` read on wake: the full transcripts stay on disk and the session ingests one bounded findings envelope instead of N transcripts. **If a leg crashed (Codex `rc=1`/`app-server exited unexpectedly`), re-dispatch ONLY the crashed leg(s) -- the envelope's `needs_redispatch` list names exactly which, and its exit-0 means `all_clean` (all legs completed AND rc==0); reuse the already-clean legs' artifacts and NEVER re-run the whole 3-leg bundle over one crash.**

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

   **Reception cadence -- ONE reception per WAVE, not one per leg (T1-2b).** A wave is the set of legs you dispatched together and waited on together: step 5's adversarial is a wave of one; 8a + 8b are a single wave of two; the unattended broker bundle is one wave of three. Wait for every leg in the wave to complete, read the ONE combined `review-envelope.py --todo <this-section's-todo-path>` envelope, then apply `superpowers:receiving-code-review` ONCE across every finding in it.

   Each dispatch does fire its own PostToolUse reminder -- that is the harness prompting per Bash call, not a per-leg obligation. Do NOT invoke the skill once per reminder. The mechanism makes this exact: `codex_review_completed.py` OVERWRITES `.claude/state/last-codex-review.json` on every trigger (a single record, no per-kind accumulation), so legs dispatched in one parallel message collapse to one `received: false` record that ONE reception clears. Receiving per leg re-injects the ~6.2 KB discipline body once per dispatch for no added rigor -- measured 2026-07-27: 75 of 102 `receiving-code-review` invocations landed within 30 min of the previous one, ~465 KB of pure repetition.

   This changes the CADENCE, never the RIGOR: every finding from every leg still goes through Fix / Reject / Accept with file:line verification, and reading the legs together is strictly better for spotting the same defect reported by two reviewers. If a leg's findings need fixing before another leg can even be scoped, that leg is its own wave -- receive it alone and say why.

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

    **Field rules are the canonical repo-wide stamp grammar and live in [references/stamp-fields.md](references/stamp-fields.md)** -- `implement-todo-section`, `verify-todo-section`, and `quality-review-section` all defer to that file. Read it before writing a stamp. The four rules violated most often, kept here so they are never missed:

    - **Severity tag `[Critical]`/`[H]`/`[M]`/`[L]` is REQUIRED** on every `Accepted:` / `Deferred:` line, and the `(item: "<name>" at line N)` parenthetical is MANDATORY (hook-enforced -- see the step 15 grammar). Domain-qualify every XREF (`02-kernel-core/TODO-17`, never bare `TODO-17`).
    - **`Accepted:` = out-of-scope, owner elsewhere. `Deferred:` = in-scope for this TODO, owner later.** Pick the right one; the split is what makes triage greps work.
    - **If there are no accepted/deferred items, OMIT both lines** -- never write `Accepted: none`.
    - **Verify the `> **Notes:**` block is present and canonical-shape**, and TRIM it back if it is over-stuffed. If `notes_bloat_check.py` fires during the review, that is the signal to delete content, not to opt out via `SKIP_REVIEW_HOOK`.

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

## Additional Resources

Read on demand -- the step that needs one names it inline.

- [references/stamp-fields.md](references/stamp-fields.md) -- step 16: the canonical repo-wide stamp grammar (field rules, Accepted-vs-Deferred semantics, evidence-token vocabulary, `<details>` escape hatch, stamp-region hygiene). Other skills defer here for stamp shape.
- [references/fix-loop.md](references/fix-loop.md) -- step 6: the incidents behind the self-diff gate, the test-only lighter path, the convergence gate scope, and the standing evidence map.

## TODO-08 §10 step-state telemetry

The skill-step-observer hook records each step on its real tool call; the skill-step-block hook BLOCKs commit if any required terminal step's evidence is missing. There is no "I did it inline" shortcut -- the hook does not see narration. The hook fires on `Bash(git commit:*)` and `Skill(review-todo-section)`. Required terminal steps for this skill are listed in `.claude/hooks/skill_step_map.py`. Opt-out (legitimate revert / stamp-only flows): `SKIP_SKILL_STEP_BLOCK=1 SKIP_SKILL_STEP_BLOCK_REASON="<text >= 12 chars>"`.
