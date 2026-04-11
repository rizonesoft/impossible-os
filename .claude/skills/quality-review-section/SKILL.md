---
name: quality-review-section
description: Deep quality review of an implemented TODO section -- industry standards compliance, optimization opportunities, architectural fitness, and Win11/Linux parity analysis. Unlike verify-todo-section (compliance audit), this skill asks "is it done RIGHT?" not just "is it done?" Use after verification passes, or when you want to improve an already-working section.
---

# Quality Review Section

## Execution Discipline

> This is a QUALITY audit, not a compliance audit. The section already works and is verified. The question is: does it match industry standards, is it optimally implemented, and could it be done better?
> - **Do not downgrade checklist items.** This is not verify-mode. All items stay as-is.
> - **Apply `superpowers:receiving-code-review` to EVERY Codex finding.** Verify before acting -- reject wrong findings with evidence.
> - **Fix all valid findings.** Spec violations, best practices, dead code, parity gaps -- if it's real, fix it. Only Accept findings that are wrong, irrelevant, or need missing infrastructure.
> - **One section = one commit.** All fixes committed together.

## Workflow

1. **Read the section + source files** -- full section text, all source files that implement it. Understand not just WHAT it does but HOW it does it.

2. **Industry standards research** -- for each major feature in the section, research how it SHOULD work per the relevant specification:
   - **UEFI code:** UEFI Specification 2.10+ (table formats, calling conventions, memory ownership rules, error handling requirements)
   - **ACPI code:** ACPI Specification 6.5+ (table signatures, checksums, revision handling, GAS parsing rules)
   - **SMBIOS code:** SMBIOS Specification 3.x (structure walking, string extraction, type-specific field encoding)
   - **Win32/NT code:** Windows Internals, ReactOS reference, MSDN documentation (NTSTATUS codes, parameter validation, privilege requirements)
   - **x86-64 code:** Intel SDM / AMD APM (MSR usage, CPUID leaf requirements, CR register rules, interrupt handling)

3. **Win11/Linux parity analysis** -- concrete function/file references, not vague comparisons.

4. **Codex comprehensive review** (MANDATORY -- NO EXCEPTIONS) -- single dispatch covering performance, consistency, AND dead code. If Codex responds with "no diff available", re-dispatch with actual file content (read 100-200 relevant lines). A shallow response requires re-prompting with specific angles. This step CANNOT be replaced with self-review.

   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<comprehensive prompt>"
   ```

   **CRITICAL -- Performance angles (mandatory in every prompt):**
   - Allocations in hot paths (ISR, per-tick, per-syscall)
   - O(n^2) algorithms where O(n) or O(log n) suffices
   - Spinlock/mutex hold times -- how much work under each lock?
   - Cache-hostile access patterns (struct layout vs access order)
   - Redundant computation in loops
   - Byte-at-a-time operations where bulk copies exist

   **CRITICAL -- Consistency angles (mandatory in every prompt):**
   - Struct layout matches between header, implementation, bootloader, and test
   - Constants defined in one place, used consistently everywhere
   - API contracts: does the header promise something the implementation doesn't deliver?
   - Error code consistency: same error mapped to same NTSTATUS everywhere

   **CRITICAL -- Dead code angles (mandatory in every prompt):**
   - Unreachable functions (declared in header, never called)
   - Unused defines, constants, macros
   - Orphaned types (struct defined, never instantiated)
   - Stale declarations after refactoring

   After Codex responds, apply `superpowers:receiving-code-review` to EVERY finding:
   - **Verify technically first.** Read the cited code. Is it correct?
   - **Valid:** classify and fix.
   - **Wrong/misleading:** reject with concrete code evidence.
   - **Out of scope:** accept with domain-qualified XREF.

5. **Self-review BEFORE fixing** -- walk the section's source files one more time looking for what BOTH your spec research AND Codex may have missed:
   - Regressions from recent changes in other sections
   - Race conditions that only manifest under specific scheduling
   - Edge cases at boundary values (0, 1, MAX, overflow)
   - Resource leaks on error paths (handles, memory, locks not released)
   - Industry conventions: are we doing something no other OS does? If so, is it intentional or an oversight?

6. **Classify all findings** (from steps 2, 3, 4, and 5):

   | Label | Meaning | Action |
   |-------|---------|--------|
   | **Spec violation** | Code contradicts a published specification | Fix -- this is a bug |
   | **Optimization** | Measurably better approach exists | Fix if on hot path; Accept if cold path |
   | **Best practice** | Industry convention we should follow | Fix |
   | **Simplification** | Same behavior with less code | Fix |
   | **Parity gap** | Win11/Linux does this better | Fix if feasible; Accept with XREF if large |
   | **Dead code** | Unreachable or unused | Remove |
   | **Accept** | Wrong, irrelevant, or needs missing infrastructure | Document why with code evidence |

7. **Fix loop** -- fix ALL non-Accept findings. Priority: spec violations > dead code > best practices > simplifications > parity gaps > optimizations. Build after each batch.

8. **Post-fix self-review** -- after all fixes applied:
   - Walk every fix: does it introduce regressions, new races, or new complexity?
   - Confirm no TODO/FIXME/HACK introduced by fixes
   - Final build: `bash scripts/build.sh`, confirm `=== BUILD OK ===`.

9. **Report** -- summarize what was fixed vs accepted.

10. **Quality stamp** -- add IMMEDIATELY after the Verified stamp (no blank line):
    ```
    > **Quality reviewed:** YYYY-MM-DD -- <summary>. Accepted: <items with domain-qualified XREFs, or "none">.
    ```

11. **Commit and push** -- `"quality: <TODO> §N -- <summary>"`

## Guardrails

- `superpowers:receiving-code-review` applies to EVERY Codex finding. No blind implementation.
- Do not optimize without evidence the path is hot.
- Do not sacrifice correctness or clarity for micro-optimization.
- Do not turn this into a rewrite. Targeted improvements only.
- Cite Win11/Linux sources by function/file name, not vague claims.
