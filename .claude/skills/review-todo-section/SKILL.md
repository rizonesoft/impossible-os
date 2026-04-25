---
name: review-todo-section
description: Full review of a TODO section -- adversarial Codex, consistency, performance, domain code quality, feature completeness. Runs after implementation or standalone. No steps skipped.
---

# Review TODO Section

> **External-Reviewer Contract:** This skill dispatches Codex as part of its workflow. Every finding goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot).

> **NO CODE SHIPS WITHOUT BEING EXAMINED FROM EVERY ANGLE.**
>
> You have been observed cutting corners: skipping Codex dispatches, self-reviewing instead of dispatching, accepting "clean" without running quality gates, folding separate concerns into a single shallow pass. This skill exists because your judgment about which steps to skip has been wrong repeatedly.
>
> Every step runs. Every time. Mechanically. No judgment calls about skipping.
>
> Technical completion is not enough. This review must also catch **false completeness**: code that satisfies the listed checklist items but still leaves the feature obviously incomplete, poorly wired, uncompetitive, or missing a tracked owner for adjacent work.

## Pipeline

### Phase 1: Evidence + Verify

1. **Evidence map** -- for each `[x]` item, prove via Grep/Read: "claim -> file:line". Downgrade on regression.
2. **Scope-gap audit** -- grep for `TODO`, `FIXME`, `HACK`, `STATUS_NOT_IMPLEMENTED`. Standalone stubs: implement. Infrastructure stubs: Accept with XREF.
3. **Test checkpoint verification** -- read the Test checkpoint paragraph. Grep source for each expected serial/klog message.
4. **Build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`.

### Phase 2: Adversarial Review

5. **MANDATORY Codex adversarial review** -- dispatch to Codex. No exceptions.
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<prompt>"
   ```
   **Mandatory angles:** integer overflow, buffer overread, NULL deref, SMP races, resource leaks, ABI mismatch, bounds on untrusted data.
   Apply `superpowers:receiving-code-review` to EVERY finding. **Codex can be wrong** -- it reads code without runtime context. For EACH finding:
   - **Verify first.** Read the actual code at file:line. Check callers, locks, reachability. Does the finding match reality?
   - **If valid:** fix root cause, rebuild. No performative agreement -- just fix it.
   - **If wrong:** reject with code evidence (path unreachable, lock already held, buffer bounded). Do NOT blindly fix a wrong finding.
   - **If out of scope:** accept with domain-qualified XREF. Must truly need missing infrastructure.
6. **Fix adversarial findings** -- fix all valid Critical/High/Medium. Rebuild.

### Phase 3: Quality Audit

7. **Domain code quality gates** -- INVOKE the domain-appropriate skill and walk its gates against the code:
   - `src/boot/` -> read and walk `boot-code-quality` (all 10 gates)
   - `src/kernel/` -> read and walk `kernel-code-quality` (all 10 gates)
   - `src/desktop/` -> `desktop-code-quality` | `src/shell/` -> `shell-code-quality` | `user/` -> `userland-code-quality`
   Record any gate failures as findings.

8. **MANDATORY Codex quality review -- TWO separate focused dispatches.** No exceptions, no combining. A combined "both in one prompt" dispatch is rejected: Codex's attention budget gets diluted across the angles and depth drops. Run the two dispatches sequentially; each one should focus entirely on its named angle with file:line prompts specific to that angle's class of bugs.

   > [!NOTE]
   > **Dead-code Codex retired 2026-04-25.** The dead-code dispatch was removed because its highest-risk failure mode (Codex flags a function that a future TODO section will call, the reviewer removes it, the later section has to recreate it) is not mitigated by the skill's post-hoc guardrails. Static caller-graph analysis cannot distinguish "legitimately orphan" from "reserved for §N+M," and rubber-stamped findings introduce regressions that build green and surface weeks later via SSDT / IDT / driver-vtable paths. Dead-code analysis is now out-of-band advisory work owned by a human sweep, not part of the section-ship gate.

   **8a. Consistency dispatch.** Invoke `codex-consistency-audit` via the plugin:
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<consistency prompt>"
   ```
   Angles: struct layout must match byte-for-byte across kernel + bootloader mirrors, constants defined in one place (no silent duplication with drift potential), API contracts (signature + error-code semantics match consumer expectations), ABI schemas (NVRAM variable name + GUID + attrs + size match producer/consumer), SSDT row ↔ function ↔ registration consistency, Win32 vs NT semantics, same narrowing / truncation / padding patterns applied uniformly across cross-file changes.

   **8b. Performance dispatch.** Invoke `codex-perf-review` via the plugin:
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<perf prompt>"
   ```
   Angles: allocations in hot paths / ISR contexts, O(n²) or worse on unbounded inputs, spinlock hold times spanning I/O or serial writes, byte-at-a-time operations that should be memcpy'd, branch-misprediction hazards on hot paths, cache-line sharing across per-CPU state, inline-asm barriers placed conservatively vs required.

   Apply `superpowers:receiving-code-review` to EVERY finding from EVERY dispatch. Same rules as step 5 -- **Codex can be wrong.** Verify each finding at file:line. Reject wrong findings with code evidence. Do not blindly implement. Do not blindly accept.

   **Rationale for two dispatches instead of one combined.** The one-dispatch combined prompt was valid until ~2026-04-24; observed failure mode was that Codex would deep-dive whichever angle had the most text in the prompt and skim the others. Focused per-angle dispatches give each one its own attention budget, lower the probability of missed findings, and make `Codex 3x` (adversarial + consistency + perf) the scannable baseline counter in the Quality reviewed stamp.

9. **Industry standards + Win11/Linux parity** -- spec compliance, concrete function/file references.

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
    - Codex line: `Nx` is dispatch count (minimum 3 per current rules: adversarial + consistency + perf). `<kinds>` names dispatches (`adversarial`, `consistency`, `perf`; the legacy combined `quality` kind is no longer accepted -- split it into explicit entries; the legacy `dead-code` kind was retired 2026-04-25 and MUST NOT appear in new stamps). Add `design` or `re-adversarial` when extra passes land. Findings as `<H>H+<M>M+<L>L fixed[, <D> open][, <N>M accepted-XREF]`; drop zero terms. **Keep the count terse: `6H+5M+7L fixed, 2M accepted-XREF` is the right shape; `6H+5M+7L fixed (4H+3M+3L design adopted pre-code, 1H+1M+1L implement adversarial, ...)` is OVER-DETAILED -- per-dispatch breakdowns belong in the commit message, not in the stamp.** `scope:` names the domain code-quality skill applied OR `N/A (<reason>)` like `N/A (docs-only)`. Do NOT write the full "No domain code-quality skill applies (...)" sentence.
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
- `superpowers:receiving-code-review` on every Codex finding.
- All stamps use domain-qualified XREFs.
- All stamps include test runner bat file and expected results.
- No blank line between stamps.
