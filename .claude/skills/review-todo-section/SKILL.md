---
name: review-todo-section
description: Full review of a TODO section -- adversarial Codex, dead code, consistency, performance, domain code quality, feature completeness. Runs after implementation or standalone. No steps skipped.
---

# Review TODO Section

> **NO CODE SHIPS WITHOUT BEING EXAMINED FROM EVERY ANGLE.**
>
> You have been observed cutting corners: skipping Codex dispatches, self-reviewing instead of dispatching, accepting "clean" without running quality gates, folding separate concerns into a single shallow pass. This skill exists because your judgment about which steps to skip has been wrong repeatedly.
>
> Every step runs. Every time. Mechanically. No judgment calls about skipping.

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

8. **MANDATORY Codex quality review** -- single dispatch covering dead code + consistency + performance. No exceptions.
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<prompt>"
   ```
   **CRITICAL -- Dead code (mandatory):** unreachable functions, unused defines, orphaned types, stale declarations.
   **CRITICAL -- Consistency (mandatory):** struct layout matches, constants in one place, API contracts, error code mapping.
   **CRITICAL -- Performance (mandatory):** allocations in hot paths, O(n^2), lock hold times, byte-at-a-time ops.
   Apply `superpowers:receiving-code-review` to EVERY finding. Same rules as step 5 -- **Codex can be wrong.** Verify each finding at file:line. Reject wrong findings with code evidence. Do not blindly implement. Do not blindly accept.

9. **Industry standards + Win11/Linux parity** -- spec compliance, concrete function/file references.

10. **Feature completeness** -- grep for `STATUS_NOT_IMPLEMENTED`, partial implementations, dead API promises.

11. **Test coverage check (CRITICAL)** -- verify unit tests exist for this section:
    - Grep for the section's functions/features in `src/kernel/test/test_*.c`. If no test file covers the section's code, flag as a finding.
    - Check that the correct `TEST_CAT_*` category is used (not piggy-backed onto an unrelated category).
    - Verify a `scripts/debug/run-<category>-tests.bat` file exists for the test category.
    - If tests are missing: either add them during this review (for simple read-only checks) or flag with `**Test gap:** <description> -- needs test_<name>.c` in the stamp.
    - If no kernel-side testable surface exists (pure UEFI bootloader code), verify the TODO section has a `**Note:** No kernel test surface -- validation via serial log on WHPX.` note.

    > **Incident 2026-04-12:** TODO-03 §1-§12 and TODO-17 §1-§3 all shipped without unit tests. The TODO-19 §1 review then found 3 critical FPU bugs that tests would have caught. Tests are not optional -- they catch real bugs.

12. **Self-review** -- regressions, races, edge cases, resource leaks.

13. **Fix ALL findings** from steps 7-12. Priority: spec violations > incomplete stubs > missing tests > dead code > quality gate failures > consistency > parity > perf. Rebuild after each batch.

### Phase 4: Stamp + Commit

14. **Reconcile tables** -- IO row, OS Comparison row.

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

16. **Stamps** -- Verified + Quality reviewed. No blank line between. Domain-qualified XREFs in Accepted field. Each Accepted XREF must reference a concrete item per step 15. Include test runner info:
    ```
    > **Test runner:** `scripts\debug\run-<category>-tests.bat` (SUITE=<cat>), N suites, 0 failures expected
    ```

17. **Commit and push** -- `"review: <TODO> §N -- <summary>"`. If step 15 created or modified items in other TODO files, stage and commit those in the SAME commit as the stamp.

## Rules

- **TWO Codex dispatches per review.** Step 5 (adversarial) and step 8 (quality). Both mandatory.
- **Domain code quality skill walked explicitly** in step 7. Not just the hook -- read the skill and check every gate.
- **Test coverage verified** in step 11. Missing tests are a finding, not acceptable.
- `superpowers:receiving-code-review` on every Codex finding.
- All stamps use domain-qualified XREFs.
- All stamps include test runner bat file and expected results.
- No blank line between stamps.
