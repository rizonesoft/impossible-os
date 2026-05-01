---
name: implement-todo-item
description: Execute a single `[ ]` checklist item (or close a `[/]` partial) within an existing TODO section. Lighter ceremony than implement-todo-section because the scope is one bullet, not the whole section. Use when a section is mostly shipped and one or two items remain, OR a gap-audit follow-up filed a concrete `[ ]` that needs to land without relaunching the full section pipeline.
---

# Implement TODO Item

> **External-Reviewer Contract:** This skill dispatches Codex as part of its workflow. Every finding goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

> **Distinct from `implement-todo-section`:** that skill handles a whole new section (design dispatch + multi-round impl-adversarial + section-ship stamp). This skill handles one bullet at a time, with proportional ceremony. If the item you need to land touches more than one section, more than ~200 LOC, or introduces a new subsystem boundary, **stop and use `/implement-todo-section` instead.**

## When to Use

Use this skill when:
- A TODO section is `[/]` (partial) with one or two `[ ]` items left.
- A gap-audit follow-up filed a concrete `[ ]` (e.g. "boot_loader_identity Notes block", "BOOT_LOADER_BUILD_LABEL escape", "vfs_close on diag_dir node") that needs to ship without re-doing the whole section's review pipeline.
- A `[/]` item has its remaining work in clear sight and can close in one focused fix (e.g. "padlock icon when `g_system_state.secure_boot` published" if the consumer side is now ready).

Do NOT use this skill when:
- The section is `[ ]` (not started) -- use `/implement-todo-section`. A first-item commit on a new section needs the design dispatch + IO-row promotion + section-ship stamp pipeline.
- The item touches > 200 LOC of net-new code, multiple subsystems, or a new ABI/struct -- those are section-class changes.
- The item description says "Add new section ..." or "Create new TODO ..." -- structural changes use `/create-todo` or `/implement-todo-section`.
- The item is the LAST `[ ]` AND the section has accumulated review-pipeline gaps (no Notes block, stale Test runner line) -- closing the section warrants the full review skill, not a one-liner.

## Execution Discipline

> **Smaller scope, same rigor.** Single-item scope does NOT mean skip-the-review. It means one focused adversarial dispatch instead of a multi-round design + impl pipeline. Every Codex finding still goes through `superpowers:receiving-code-review`; every domain code-quality gate still gets walked.
> - **Honest scope check at step 1.** If during exploration the item turns out to be larger than advertised, STOP and switch to `/implement-todo-section`. Do not let "I'm already in" pressure override the size cap.
> - **No silent broadening.** Items adjacent to the listed bullet that you discover need work either ship inline (when same-subsystem and small) or get filed as concrete `[ ]` items in the section -- never left in a comment.
> - **Auto-promotion to section-ship when this is the last item.** If this item flips the section to fully `[x]`, the workflow upgrades to a section-commit (Notes block + Verified stamp + Implementation Order row flip + post-commit `/review-todo-section`). The size of the change does not change; the close-out paperwork does.

## Workflow

1. **Read the item in context.** Read the full section: the item's bullet, its neighbors, the Test checkpoint, any `> [!NOTE]` / `> [!WARNING]` callouts, and the existing Notes block (if the section is `[/]` it should already have one). Note the section number and its Implementation Order row status.
2. **Scope-cap check.** Estimate the change size: lines of code, files touched, subsystems crossed. If your estimate exceeds **200 LOC OR 4 files OR 2 subsystems**, STOP and report in chat: "this item is section-class; switching to `/implement-todo-section`." Do not proceed under this skill.
3. **Resolve item-scoped XREFs.** If the bullet's prose mentions `-> XREF: TODO-XX §N` or names a function/struct from another section, follow each link and confirm prerequisites are `[x]`. If a prerequisite is open, document the blocker in chat and stop.
4. **Explore the existing code surface.** Grep/Read for the symbols, functions, and files the item names. Confirm the item's described approach matches what's in the tree today (sometimes the item description has drifted from reality and the bullet should be rewritten to match shipped code instead of implemented).
5. **Walk the domain code-quality skill.** `Skill(<area>-code-quality, ...)` based on the file path the item touches (`src/boot/` -> `boot-code-quality`, `src/kernel/` or `include/kernel/` -> `kernel-code-quality`, etc.). The hook auto-binds; the gate checks every applicable rule. Single-item scope does not waive this.
6. **Implement.** Same constraints as `implement-todo-section`:
   - Freestanding kernel: `#include "kernel/types.h"` only; no `<stdint.h>`/`<string.h>`/`<stdlib.h>`.
   - No `malloc()`/`printf()` -- use `kmalloc()` (<= 4 KB) or `pmm_alloc_contiguous()` and `printk()`/`klog()`.
   - SMP-safe by default (spinlocks, atomics, per-CPU as appropriate).
   - POST16 codes are BOOT-PATH ONLY (Phase 0/1/2 init); post-Phase-3 paths use `klog()`.
   - **Scope-gap protocol still applies.** If you find yourself reaching for a `// TODO`/`// FIXME` comment, walk the [scope-gap protocol](../implement-todo-section/scope-gap-protocol.md) Branches A/B/C/D. Do not bury new gaps in comments to keep the item bounded.
7. **Build.** `bash scripts/build.sh`; confirm `tail -1 build/build.log` shows `=== BUILD OK ===`.
8. **Wire / extend tests** when the item changes runtime behavior. New public function -> at least one assertion in the matching `test_*.c`. New struct/enum field -> `_Static_assert` for layout pin AND a runtime offset assert in tests. Item-scoped changes to existing code paths often don't need new tests -- existing tests already cover the path; verify the existing test still asserts the property the item changes. **Forbidden in tests** (per `feedback_test_no_live_boot_calls`): `boot_progress`, `vpd_*`, `panic`, `boot_halt`, any subsystem `_init`. Pre-commit hook enforces.
9. **Codex adversarial dispatch (DEFAULT MANDATORY; trivial-fix opt-out below).** Dispatch a focused adversarial review on the item's diff:
   ```bash
   bash scripts/codex-dispatch.sh '[review-kind: adversarial] todo/<domain>/TODO-XX-<slug>.md §N <item one-line summary>; files in scope: <comma-separated>; angles: integer overflow, buffer overread, NULL deref, SMP races, resource leaks, ABI mismatch, bounds on untrusted data.'
   ```
   Receive findings via `Skill(superpowers:receiving-code-review, ...)`. Fix valid C/H/M; reject false positives with code evidence; accept out-of-scope with concrete `[ ]` XREF.

   **Trivial-fix opt-out (rare; declare in chat which case applies):**
   - **Docs-only**: only `.md` / `docs/` files touched. No code surface to review.
   - **Constants-only**: a single `#define`, an enum value addition, a registry-table-row addition under existing `pe_export_entry_t` style.
   - **Test-only**: a new assertion in an existing `test_*.c` with no new helper.
   - **Stamp/wording-only**: TODO checklist text rewrite, Verified-stamp count update, Notes-block typo fix.

   When opting out, set `SKIP_CODEX_REVIEW=1 SKIP_CODEX_REVIEW_REASON="<criterion + 1-line evidence>"` on the next tool call AND state the criterion in chat. The hook honors the env-prefix in the same shape as other skip variables. **"Looks small" is NOT a criterion.** A 30-LOC change to scheduler logic is not trivial; a 200-LOC docs sweep is.
10. **Self-review (cheap, mandatory).** Re-read the diff once with these questions: regressions, races, edge cases, resource leaks, parallel arrays out of sync, exports table missing the new symbol. Most of these don't apply to a single-item change but stating them out loud catches the rare slip. One short paragraph in chat.
11. **Build again** if step 9 produced fixes. `=== BUILD OK ===` required before commit.
12. **Update the TODO file:**
    - **Flip the `[ ]` (or `[/]`) to `[x]`.** Rewrite the bullet to describe **what shipped**, not what the original draft proposed. Cite filenames + key symbol names + brief one-line rationale. Per `feedback_todo_notes_brevity`, the per-finding evidence trail lives in the commit message; the `[x]` line is a canonical record of shipped code.
    - **If this is the LAST `[ ]` in the section** -- auto-promote to section-ship:
        - Update the section's Implementation Order row from `[/]` to `[x]`.
        - If the section's Notes block is missing or stale, add/refresh it per the canonical 5-bullet shape (what shipped / how it integrates / downstream effects / canonical doc / scope boundary). One line per bullet.
        - Add a `> **Verified:**` stamp line under the Notes block following the field rules in `review-todo-section` step 16. The Codex count reflects the dispatches that ran (1 for adversarial-only items; more if this item required multiple rounds).
    - **If the section is still `[/]` after this commit** -- no Notes-block change, no Verified stamp, no IO-row flip. The section remains in-progress.
13. **Tie up loose ends.**
    - Inbound `Accepted:` / `Deferred:` stamps from other TODOs that pointed at this item -- if the gap is fully closed, delete those lines per `implement-todo-section` step 18 (`python3 scripts/todo-graph/query.py deferred-by <id>` enumerates inbound XREFs).
    - Cross-TODO sync: if this item closes a dependency another TODO names, patch the back-reference there in the same commit.
    - PE-export tables (`pe.c`'s `s_kernel32_exports[]` / `s_ntdll_exports[]`) when the item adds a Win32-shaped public API.
    - Parallel arrays (anything indexed by an enum that just grew).
14. **Commit.** Single commit, item-scoped subject:
    - **Item-only (section still `[/]`):** `<area>: <one-line item summary>` (e.g. `boot: vfs_close on X:\Diag\ node in boot_version_blackbox_transcribe`).
    - **Item closes section (auto-section-ship):** `<area>: <section title> -- <last-item summary>` mirroring the section's `Commit:` line if one is named there.
    - Body: list the item bullet, evidence cited (file:line for each change), Codex finding adoptions if any, and verification (`build OK; tests N/M PASS; lint exit 0`).
15. **Push** to `origin/main`.
16. **Post-commit review (CONDITIONAL):**
    - **If step 12 flipped the section to fully `[x]`**: invoke `Skill(review-todo-section, "todo/<domain>/TODO-XX-<slug>.md §N <title>")` as the LITERAL next tool call. The post-ship gate (`section_review_required.py`) will block subsequent edits/writes until review runs. This is non-negotiable; section-ship without `/review-todo-section` is the failure mode the hook was built to prevent.
    - **If the section is still `[/]`**: no review skill required. Single-item commits accumulate; the next agent that closes the section runs the review on the cumulative state.

## Hard Gates

- **Step 7 + step 11 (build OK)** are non-skippable. A pending-merge fix in head-of-tree is not a license to commit a broken build.
- **Step 9 (Codex adversarial)** is mandatory by default. The trivial-fix opt-out exists but is narrow; declare which criterion applies in chat AND set the SKIP env on the same call.
- **Step 14 (commit)** must be a SINGLE commit -- do not stack a review-pipeline commit + a fix commit in the same flow. If review-pipeline findings need fixes, they go in step 9's fix loop, not in a separate commit.
- **Step 16 review-skill auto-invocation** when the section closes is non-skippable. The hook enforces.

## Relationship to other skills

- `implement-todo-section`: superset workflow for a whole new section. Use when scope > one item.
- `review-todo-section`: post-section-ship quality pipeline. This skill INVOKES it at step 16 when a section auto-closes.
- `verify-todo-section`: audit-mode wrapper over review (downgrade-only).
- `complete-todo-file`: full-file finalization across all sections; orthogonal scope.
- Domain code-quality skills (`boot-code-quality`, `kernel-code-quality`, etc.): walked at step 5.
- `superpowers:receiving-code-review`: applied to every Codex finding from step 9.

## Telemetry / hooks

The skill is wired into `skill_step_map.py` with terminal steps `[7, 9, 11, 14]` (first build, adversarial dispatch, second build, commit). The `skill_step_block.py` gate refuses commit when any of these is missing, with the trivial-fix opt-out for step 9 honored via `SKIP_CODEX_REVIEW=1` plus a 12-char-minimum reason. When step 12 promotes to a section-ship, `section_commit_gate.py` adds its standard four-dispatch evidence requirement (the post-impl review pipeline catches that the agent must follow with `/review-todo-section`).
