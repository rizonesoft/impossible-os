---
name: quality-review-section
description: Deep quality review of an implemented TODO section -- industry standards compliance, optimization opportunities, architectural fitness, and Win11/Linux parity analysis. Unlike review-todo-section (post-implementation quality sweep with stamps) or verify-todo-section (audit-mode over review), this skill asks "is it done RIGHT?" -- deep improvements to already-working code. Use after review passes, or when you want to improve an already-working section.
---

# Quality Review Section

> **External-Reviewer Contract:** This skill dispatches Codex as part of its workflow. Every finding goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

## Execution Discipline

> This is a QUALITY audit, not a compliance audit. The section already works and is verified. The question is: does it match industry standards, is it optimally implemented, and could it be done better?
> - **Do not downgrade checklist items.** This is not verify-mode. All items stay as-is.
> - **Apply `superpowers:receiving-code-review` to EVERY Codex finding.** Verify before acting -- reject wrong findings with evidence.
> - **Fix all valid findings.** Spec violations, best practices, dead code, parity gaps -- if it's real, fix it. Only Accept findings that are wrong, irrelevant, or need missing infrastructure.
> - **One section = one commit.** All fixes committed together.
> - **Think beyond parity.** This pass asks whether the result is refined, competitive, and worthy of Impossible OS, not merely equivalent to the current checklist text.

## Workflow

1. **Read the section + source files** -- full section text, all source files that implement it. **Dispatch `Agent(subagent_type="review-evidence-mapper", ...)` BY DEFAULT first** for the file:line evidence map of the implementation surface (skip only for tiny stamp-only/docs sections), then read the load-bearing files it points at yourself. Understand not just WHAT it does but HOW it does it.

2. **Industry standards research** -- for each major feature in the section, research how it SHOULD work per the relevant specification:
   - **UEFI code:** UEFI Specification 2.10+ (table formats, calling conventions, memory ownership rules, error handling requirements)
   - **ACPI code:** ACPI Specification 6.5+ (table signatures, checksums, revision handling, GAS parsing rules)
   - **SMBIOS code:** SMBIOS Specification 3.x (structure walking, string extraction, type-specific field encoding)
   - **Win32/NT code:** Windows Internals, ReactOS reference, MSDN documentation (NTSTATUS codes, parameter validation, privilege requirements)
   - **x86-64 code:** Intel SDM / AMD APM (MSR usage, CPUID leaf requirements, CR register rules, interrupt handling)

3. **Win11/Linux parity and superiority analysis** -- concrete function/file references, not vague comparisons.
   - What do Win11 and Linux already do here?
   - Where is Impossible OS still behind?
   - Where can Impossible OS be cleaner, faster, safer, or more coherent without turning this into a rewrite?

4. **Codex comprehensive review** (MANDATORY -- NO EXCEPTIONS) -- single dispatch covering performance, consistency, AND dead code. If Codex responds with "no diff available", re-dispatch with actual file content (read 100-200 relevant lines). A shallow response requires re-prompting with specific angles. This step CANNOT be replaced with self-review.

   ```bash
   bash scripts/codex-dispatch.sh '[review-kind: adversarial] <todo-path> <comprehensive prompt>'
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

5. **Feature completeness + adjacent completeness audit** (MANDATORY) -- grep the section's source files for incomplete features and "almost real" implementations:
    - **`STATUS_NOT_IMPLEMENTED` stubs:** if the function is standalone (self-contained, under 1000 lines), IMPLEMENT it fully. Features must not get lost behind stubs. If it requires significant new infrastructure, Accept with a domain-qualified XREF.
    - **Scope-gap markers:** `TODO`, `FIXME`, `HACK`, `XXX`, `for now`, `placeholder`, `STUB(`. Cross-check against TODO text -- undisclosed gaps are findings.
    - **Partial implementations:** functions that return early on edge cases without handling them. Error paths that silently succeed. Switch statements with missing cases.
    - **Dead API promises:** functions declared in headers but not implemented, or implemented but never called.
    - **Adjacent completeness:** exports, registrations, tables, tests, docs, call paths, or owner TODO items missing from an otherwise "done" feature.
    - **Product feel gaps:** places where the section technically works but still feels rough, under-designed, or clearly behind Win11/Linux in a way that is feasible to improve now.

7. **Self-review BEFORE fixing** -- walk the section's source files looking for what spec research, Codex, AND the stub audit missed:
    - Regressions from recent changes in other sections
    - Race conditions that only manifest under specific scheduling
    - Edge cases at boundary values (0, 1, MAX, overflow)
    - Resource leaks on error paths (handles, memory, locks not released)
    - Completion radar: correctness, completeness, wiring, parity, superiority, ownership

8. **Classify all findings** (from steps 2, 3, 4, 5, and 7):

   | Label | Meaning | Action |
   |-------|---------|--------|
   | **Spec violation** | Code contradicts a published specification | Fix -- this is a bug |
   | **Optimization** | Measurably better approach exists | Fix if on hot path; Accept if cold path |
   | **Best practice** | Industry convention we should follow | Fix |
   | **Simplification** | Same behavior with less code | Fix |
    | **Parity gap** | Win11/Linux does this better | Fix if feasible; Accept with XREF if large |
    | **Refinement** | Code works but is still obviously rough or under-designed | Fix if feasible; Accept with XREF if larger |
    | **Dead code** | Unreachable or unused | Remove |
    | **Incomplete** | STATUS_NOT_IMPLEMENTED stub or partial implementation | Implement if standalone; Accept with XREF if needs infrastructure |
    | **Accept** | Wrong, irrelevant, or needs missing infrastructure | Document why with code evidence |

9. **Fix loop** -- fix ALL non-Accept findings. Priority: spec violations > incomplete stubs > dead code > best practices > simplifications > parity gaps > refinement > optimizations. Build after each batch.

10. **Post-fix self-review** -- walk every fix for regressions, confirm no TODO/FIXME/HACK introduced. Final build.

11. **Report** -- summarize what was fixed vs accepted.

12. **Quality stamp** -- add IMMEDIATELY after the Verified stamp (no blank line). Compact pipe-separated format -- one line of fields, not a paragraph:
    ```
    > **Quality reviewed:** YYYY-MM-DD | Codex Nx (<kinds>) | <H>H+<M>M+<L>L fixed, <D> open | scope: <skill or "N/A (reason)">
    ```
    If findings were deferred, add a `> **Accepted:**` (out-of-scope, owned elsewhere) or `> **Deferred:**` (in-scope, owned later) line on its OWN blockquote line between Verified and Quality reviewed. Each deferred line requires a severity tag `[Critical|H|M|L]` at the start and ends with a hook-enforced `(item: "..." at line N)` XREF parenthetical; optional `(reason: <short>)` when the defer-rationale is not obvious from the finding text. See `review-todo-section` SKILL.md step 16 for the full field rules, Accepted-vs-Deferred semantic split, Verified evidence-token vocabulary, and `<details>` escape hatch. Do NOT re-paste per-finding prose into the Quality-reviewed line; counts + XREFs carry the audit trail.

13. **Commit and push** -- `"quality: <TODO> §N -- <summary>"`

## Guardrails

- `superpowers:receiving-code-review` applies to EVERY Codex finding. No blind implementation.
- Do not optimize without evidence the path is hot.
- Do not sacrifice correctness or clarity for micro-optimization.
- Do not turn this into a rewrite. Targeted improvements only.
- Cite Win11/Linux sources by function/file name, not vague claims.

## TODO-08 §10 step-state telemetry

The skill-step-observer hook records each step on its real tool call; the skill-step-block hook BLOCKs commit if any required terminal step's evidence is missing. There is no "I did it inline" shortcut -- the hook does not see narration. The hook fires on `Bash(git commit:*)` and `Skill(review-todo-section)`. Required terminal steps for this skill are listed in `.claude/hooks/skill_step_map.py`. Opt-out (legitimate revert / stamp-only flows): `SKIP_SKILL_STEP_BLOCK=1 SKIP_SKILL_STEP_BLOCK_REASON="<text >= 12 chars>"`.
