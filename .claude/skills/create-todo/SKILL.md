---
name: create-todo
description: Create lean project TODO files under todo/, choose the correct domain and local number, build the canonical Implementation Order table, wire XREFs, and update indexes. Use when the user asks to create, author, scaffold, or derive a new TODO roadmap from a feature, spec, or subsystem.
---

# Create TODO

## Use This Skill When

- The user wants a new TODO file under `todo/`.
- A feature, subsystem, or spec needs a tracked execution roadmap.
- A stale draft needs to be rewritten into the current lean TODO format.
- A scope gap discovered during `implement-todo-section` (or `verify-todo-section`) step 6 needs a new TODO file because the gap spans multiple subsystems and no existing TODO covers it (scope-gap protocol Branch C). See [../implement-todo-section/scope-gap-protocol.md](../implement-todo-section/scope-gap-protocol.md). Before creating a new file, the scope-gap protocol requires a dedup sweep across all existing TODOs -- if any TODO already covers the scope, use Branch D (add to existing TODO) instead of creating a new one.

## Plan Quality Principles

> A TODO file IS an implementation plan. Apply these principles from `superpowers:writing-plans`:
> - **Identify critical files** -- list every source file the plan will touch in Inputs.
> - **Consider architectural trade-offs** -- document why this approach was chosen over alternatives.
> - **Define clear milestones** -- each `## N.` section is a milestone with a commit checkpoint.
> - **Sequence dependencies correctly** -- the Implementation Order table IS the dependency graph.
> - **Budget section complexity at authoring time.** Estimate per section: files touched, subsystems, expected LOC, ABI impact, and tests. A section likely to exceed ONE fresh worker context gets split NOW (each resulting section runs the full quality pipeline) -- splitting during implementation costs a wasted context. Rule of thumb (mirrors `scripts/overnight/section-manifest.py` complexity verdict): > 8 files, > 12 checklist items, > 3 subsystems, or ABI impact + a wide item list means split.
> - **Make it executable** -- someone (human or AI) should be able to follow §1 through §N without guessing.
> - **Plan for credible completeness** -- do not stop at the obvious happy-path steps. Include adjacent work needed so the feature feels real, wired, testable, and not one missing piece away from a scope-gap stub.
> - **Decide vendor-vs-build BEFORE writing the checklist.** For any subsystem that a mature project already implements (protocol stacks, interpreters, crypto, format parsers, font shaping, decompressors), state explicitly which way the plan goes and why. Specifying 200 checklist items that reimplement a BSD-licensed library is a decision, and it should be a conscious one. See CLAUDE.md "Vendor-First Evaluation". The license check comes first and GPL-2.0-only is a hard stop (Linux, NTFS-3G, lwext4) -- read the upstream LICENSE file, never its README. Record the verdict in the Goal or an Inputs line either way, including "from scratch because <upstream> is GPL-2.0-only", so nobody re-opens a settled question.
> - **Plan for competitive advantage** -- parity with Win11 and Linux is the floor. Capture the cleaner, faster, or more refined design choices that make Impossible OS better where that is feasible.

## Workflow

### Scope-Gap Mode (invoked from scope-gap protocol Branch C)

When `create-todo` is invoked from the scope-gap protocol, the workflow is the same numbered steps below, with these overrides:

- **Dedup-check-first is MANDATORY.** Before ANY research or drafting, run `Grep` across `todo/*/` for the gap's keywords, function names, feature names, and OS Comparison row text (~30 seconds). If a strong match is found, STOP -- switch the invoker to Branch D of the scope-gap protocol and do NOT create a new file. The dedup check is the whole reason scope-gap mode exists as a named branch. See A.5.1 in [../implement-todo-section/scope-gap-protocol.md](../implement-todo-section/scope-gap-protocol.md).
- **Inputs block seeded from originating section.** Copy the originating TODO's section files into the new TODO's Inputs block as a starting point.
- **Bidirectional XREF is automatic.** The new TODO gets a `-> XREF: <originating TODO> §N` link in its Inputs. The originating section ALREADY has its `Follow-up (Branch C -> XREF: ...)` line (added by the implement-mode Branch C step 4). Verify both sides before closing out the invocation.
- **Domain INDEX.md note.** The new TODO's INDEX.md row must include `(created from scope-gap in <originating TODO> §N)` so the paper trail is visible at the domain level.

1. **Find the canonical home.**
   - Read `todo/TODO-00-INDEX.md`, the target domain `INDEX.md`, and adjacent TODOs.
   - Prefer one canonical TODO home and cross-link instead of duplicating scope.

2. **Research the domain (critical -- do not skip).**
    - If a spec exists, treat it as the primary structural input.
    - Use Grep/Glob to discover existing implementations, related symbols, and prior art.
    - Use `git log` to discover recent work that may already cover planned scope.
    - **Win11/Linux competitive research:** dispatch `Agent(subagent_type="parity-research-analyst", ...)` in **todo-plan mode** BY DEFAULT to run this inventory in a throwaway context (it owns the searches below and returns the structured feature inventory). Fall back to in-context searching only if the dispatch fails. The requirement: at least 6 web searches (3 Win11, 3 Linux) to build a feature inventory for the OS Comparison table. Queries: `"Windows 11 <topic> internals"`, `"Linux kernel <topic>"`, `"modern OS <topic> best practices"`. Follow at least 2 links with WebFetch for detailed feature descriptions.
    - Build a feature inventory: for each feature, record Win11 status, Linux status, and whether it's parity (both have it) or competitive edge (neither does it well).
    - Build an **adjacent completeness inventory:** for each planned milestone, list the next obvious capability, wiring step, or owner TODO that would be needed so the feature does not ship as paper completion.
    - **Save research sources** for your own traceability only: list URLs in the **gap-analysis or create-todo chat report** (or commit message notes). **Do not** paste URL lists into the TODO as `<!-- Sources: ... -->` HTML comments -- they clutter diffs and duplicate long lines across files.

3. **Cross-TODO overlap scan.**
   - **Fast path:** if any planned section overlaps with an existing TODO, that TODO's `id` is the natural lookup key. Call `mcp__todo-graph__backlinks <id>` to enumerate every inbound XREF / depends_on / satisfies / Inputs reference / Accepted-stamp / Deferred-stamp pointing at the candidate target. The MCP returns one structured answer; a multi-step grep traversal approximates the same thing. Doctrine: [docs/infrastructure/mcp-usage.md](../../../docs/infrastructure/mcp-usage.md).
   - **Grep fallback** (when no candidate `id` is known yet, or for free-text overlap): use `Grep` to search ALL domain folders for keywords from the planned section titles and deliverables. Markdown body text search is the canonical case where grep beats the MCP.
   - For each match, classify: SUPERSEDES, COMPLEMENT, CONFLICT, or FOUNDATION.
   - Add bidirectional `-> XREF:` links for every real dependency or overlap.
   - **Edit both sides in the same task.** If you add a `-> XREF` to the new TODO pointing at TODO-XX, also add a reciprocal `-> XREF` in TODO-XX pointing back. One-sided XREFs are broken XREFs.
   - Prefer one canonical TODO home -- do not duplicate scope.

4. **Choose the next local filename.**
   - Follow the live domain naming pattern such as `todo/00-infrastructure/TODO-03-short-name.md`.
   - Do not revive legacy `TODO-NNN.NN-*` naming from `todo-old/`.

5. **Draft the TODO in the current lean format.**
   - Default to a leaf TODO.
   - Create a parent TODO only when the topic truly needs multiple child files or shared verification.
   - Use the template in [todo-template.md](todo-template.md) as the structural starting point.
   - **Always include the `> [!IMPORTANT] Current state:` callout** after the Goal paragraph. This tells the implementer what exists NOW vs what the TODO adds. Without it, the implementer has to grep the codebase to understand the starting point.

6. **Build sections with full structure.**
    - Use flat numbered sections (`## 1. Title`, `## 2. Title`) -- not sub-sections (`### 1.1`).
    - **Section size rule:** max 8-10 checklist items per section. If a section grows beyond this, split it into two sections. Each section should be implementable in one commit. A 15-item section is two sections pretending to be one.
    - **Completion-first section rule:** if a section would otherwise land code that is technically present but obviously incomplete, split the adjacent work into either the same section (if still one-commit sized) or the next owning section now. Do not leave the missing piece implicit.
    - **Cold-executor rule:** author every section for an executor with ZERO conversation context (the overnight runner relaunches fresh). Inline the substance -- never "per the audit" or "as discussed"; verify every file:line reference exists before writing it; replace judgment verbs ("clean up", "harden appropriately") with testable criteria; when an item embeds a high-risk default decision (ABI, security boundary, data format), tell the section explicitly to DEFER that sub-item with `awaiting-answer` and continue the rest. A cold-read of the finished section must be sufficient to implement it.
    - Every section MUST include:
      - Concrete checklist items (`- [ ]`) referencing specific functions, types, or files.
      - A `- [ ] Commit: "scope: short description"` line as the last checklist item.
      - A `**Test checkpoint:**` block with concrete pass/fail criteria: exact serial strings, expected behavior, and platforms (QEMU WHPX + TCG; bare metal). Mention POST16 codes ONLY if the section is boot-path (Phase 0/1/2). For non-boot sections, use a `klog(LOG_INFO, ...)` line as the observable instead.
   - For sections touching BOOT-PATH code (Phase 0/1/2 init, hardware bring-up, page tables, GDT/IDT, APIC, ACPI, SMP AP startup):
     - Add `POST16(0xDDNN)` entry/exit codes. Check `boot_init.h` for conflicts before assigning.
     - Add a `**Regression risk:**` note identifying what could break and rollback strategy.
   - For sections touching POST-BOOT code (scheduler, syscall handlers, exec/loader, file I/O, IPC, network, drivers used at runtime):
     - Do NOT add POST16 codes. klog is fully working at this point and a single `klog(LOG_INFO, ...)` line per major step is more useful.
     - Tests for the behavior, not for "POST16 constant equals 0xDDNN" (tautological -- the compiler enforces the literal).
    - **Cross-reference with `kernel-code-quality` gates:** If the TODO touches kernel code (`src/kernel/`, `include/kernel/`), write sections with the quality gates in mind. Note SMP safety requirements in section prose where shared mutable state is introduced. Note bare-metal gotchas where MMIO or CPUID-gated instructions are involved.
    - **Add ownership where needed:** if a section intentionally defers nearby work, add the owning follow-up section or external TODO XREF now. Never rely on vague future intent.
    - **Use callouts** for important context:
      - `> [!NOTE]` for resolved issues or clarifications
      - `> [!WARNING]` for known risks or incomplete prerequisites
      - `> [!IMPORTANT]` for blockers or critical constraints
      - `> [!TIP]` for competitive advantages (exclusive features)

7. **XREF placement rules.**
   - **Inputs section XREFs** = structural dependencies. These are TODOs/files that must exist BEFORE this TODO can be implemented. They define the starting context.
   - **Section-level XREFs** = implementation-time cross-references. These are specific sections in other TODOs that a particular section depends on or satisfies. They guide the implementer during that section's work.
   - Both types must be bidirectional (the other file must point back).

8. **Set up the Implementation Order table.**
    - Use `Implementation Order`, not `Phase-by-Phase`.
    - Mark every row `💎` (parity) or `⭐` (exclusive).
    - Use compact `§N` notation for internal dependencies, `TNN §N` for same-domain, `DNN TNN §N` for cross-domain. Never bare numbers.
    - Every `## N.` body section MUST have a corresponding table row, and vice versa.
    - Cross-TODO references MUST include specific section numbers (`T11 §1,§3`). Never bare TODO numbers.
    - Add dependencies, overlap notes, handoffs, and `-> XREF:` links while creating the file.
    - If adjacent work is intentionally deferred, the owner row must exist in the table now. No invisible future sections.

9. **Add a Format Quick Reference table (when applicable).**
   - If the TODO covers multiple formats, modes, or types (e.g., ELF/PE/EIF, AHCI/NVMe, TCP/UDP), add a compact comparison table after the Outcome section.
   - Keep it to essential differences only -- not a spec dump.

10. **Add the OS Comparison table (mandatory).**
    - Place before Unit Tests / Verification sections.
    - Header: `| ⭐ | Feature | 🪟 Win11 | 🐧 Linux | 🚀 Impossible OS |`
    - Keep cells short: status emoji + max 5 words per cell. Prefer table rows under **~150 chars** (about **50% wider** than the old ~100 target) by tightening **words**, not by removing padding spaces. **Pad every column** so pipes line up in the raw markdown; separator row uses `-` per column (min 3) matching those widths (same rule as `validate-todo-file`).
    - Mark each row `💎` (parity) or `⭐` (exclusive/superior).
    - Status: ✅ Done, ⚠️ Partial, ⬜ Planned (with `§N` reference), ❌ Not applicable.
    - Populate from research in step 2, not guesswork.
    - Add a summary paragraph after the table noting competitive position at each milestone.
    - **Do not** add `<!-- Sources: ... -->` HTML comments after the table.

11. **Add the Unit Tests section (mandatory).**
    - Place after the last numbered implementation section, before Verification.
    - Required structure:
      - `> Wire into test_runner_init() via test_register_<feature>() -- register in src/kernel/test/test_runner.c.`
      - `- [ ] Create src/kernel/test/test_<feature>.c with:` followed by concrete test case sub-bullets.
      - Each test case: specific function call + expected return value or observable state. No vague "test that X works."
      - `- [ ] Register in test_runner_init(): test_register_<feature>()`
      - `- [ ] Commit: "test: add <feature> test suite"`
    - Derive test cases from the implementation sections' deliverables.
    - **Check if a new test category is needed.** If the subsystem doesn't fit existing `TEST_CAT_*` categories, note that a new `TEST_CAT_<NAME>` enum entry, `cat_names[]`/`cat_labels[]` entries, and `make test-<name>` Makefile target are needed -- AND a new bat runner under the **right subdir** for the test layer (the bat-runner subdirs split 2026-04-20: kernel-side `TEST_CAT_*` -> `scripts/debug/kernel/run-<cat>-tests.bat`; user-mode `test_*.exe` -> `scripts/debug/usermode/run-<binary>.bat`; desktop UI -> `scripts/debug/desktop/run-<test>.bat`). Per-category bats MUST NOT live at the `scripts/debug/` root; that location is reserved for `run-all-tests.bat`. Reference the `implement-unit-tests` skill for the full procedure.

12. **Add the Verification section.**
    - Concrete platform-specific verification items, not vague "build and test."
    - Include: `bash scripts/build.sh clean` -> `=== BUILD OK ===`.
    - Include runtime verification with **specific observable outcomes**: exact serial log strings to grep for, specific function calls that should return specific values. POST16 code sequences ONLY for boot-path sections (Phase 0/1/2); for post-boot code, use klog grep targets instead.
    - Include platform list: QEMU WHPX (2 CPUs), QEMU TCG, VirtualBox, bare metal.
    - For hardware-touching TODOs: "Verify on bare metal -- VM behavior differs."

13. **Do NOT add model tags to section headings.**
    - No `[Opus]` or `[Sonnet]` postfixes -- model selection is handled by the harness, not the TODO.

14. **Update indexes in the same task.**
    - Update the domain `INDEX.md`.
    - Update `todo/TODO-00-INDEX.md` only when the new TODO changes root-visible scope.

## Guardrails

- Do not implement code or reconcile checkbox truth against the codebase.
- In scope-gap mode, ALWAYS run the dedup sweep before drafting. A new TODO created over an existing one is a documentation bug that is expensive to unwind -- Branch D exists specifically to prevent it.
- Do not add giant mutable prompt blocks, formatting-only cleanup steps, or MCP-memory instructions.
- Keep tables compact and split wide planning content into bullets or short subsections.

## Additional Resources

- [todo-template.md](todo-template.md)
- [implementation-order.md](implementation-order.md)
