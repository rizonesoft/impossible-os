---
name: create-todo
description: Create lean project TODO files under todo/, choose the correct domain and local number, build the canonical Implementation Order table, wire XREFs, and update indexes. Use when the user asks to create, author, scaffold, or derive a new TODO roadmap from a feature, spec, or subsystem.
---

# Create TODO

## Use This Skill When

- The user wants a new TODO file under `todo/`.
- A feature, subsystem, or spec needs a tracked execution roadmap.
- A stale draft needs to be rewritten into the current lean TODO format.

## Plan Quality Principles

> A TODO file IS an implementation plan. Apply these principles from `superpowers:writing-plans`:
> - **Identify critical files** -- list every source file the plan will touch in Inputs.
> - **Consider architectural trade-offs** -- document why this approach was chosen over alternatives.
> - **Define clear milestones** -- each `## N.` section is a milestone with a commit checkpoint.
> - **Sequence dependencies correctly** -- the Implementation Order table IS the dependency graph.
> - **Make it executable** -- someone (human or AI) should be able to follow §1 through §N without guessing.

## Workflow

1. **Find the canonical home.**
   - Read `todo/TODO-00-INDEX.md`, the target domain `INDEX.md`, and adjacent TODOs.
   - Prefer one canonical TODO home and cross-link instead of duplicating scope.

2. **Research the domain (critical -- do not skip).**
   - If a spec exists, treat it as the primary structural input.
   - Use Grep/Glob to discover existing implementations, related symbols, and prior art.
   - Use `git log` to discover recent work that may already cover planned scope.
   - **Win11/Linux competitive research:** Run at least 6 web searches (3 Win11, 3 Linux) to build a feature inventory for the OS Comparison table. Queries: `"Windows 11 <topic> internals"`, `"Linux kernel <topic>"`, `"modern OS <topic> best practices"`. Follow at least 2 links with WebFetch for detailed feature descriptions.
   - Build a feature inventory: for each feature, record Win11 status, Linux status, and whether it's parity (both have it) or competitive edge (neither does it well).
   - **Save research sources.** Record URLs for key findings. Add a `<!-- Sources: ... -->` HTML comment at the bottom of the OS Comparison table so the data is verifiable later.

3. **Cross-TODO overlap scan.**
   - Use Grep to search ALL domain folders for keywords from the planned section titles and deliverables.
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
   - Every section MUST include:
     - Concrete checklist items (`- [ ]`) referencing specific functions, types, or files.
     - A `- [ ] Commit: "scope: short description"` line as the last checklist item.
     - A `**Test checkpoint:**` block with concrete pass/fail criteria: exact serial strings, POST16 codes, expected behavior, and platforms (QEMU WHPX + TCG; bare metal).
   - For sections touching boot-path, interrupts, page tables, GDT/TSS, or timers:
     - Add `POST16(0xDDNN)` entry/exit codes. Check `boot_init.h` for conflicts before assigning.
     - Add a `**Regression risk:**` note identifying what could break and rollback strategy.
   - **Cross-reference with `kernel-code-quality` gates:** If the TODO touches kernel code (`src/kernel/`, `include/kernel/`), write sections with the quality gates in mind. Note SMP safety requirements in section prose where shared mutable state is introduced. Note bare-metal gotchas where MMIO or CPUID-gated instructions are involved.
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

9. **Add a Format Quick Reference table (when applicable).**
   - If the TODO covers multiple formats, modes, or types (e.g., ELF/PE/EIF, AHCI/NVMe, TCP/UDP), add a compact comparison table after the Outcome section.
   - Keep it to essential differences only -- not a spec dump.

10. **Add the OS Comparison table (mandatory).**
    - Place before Unit Tests / Verification sections.
    - Header: `| ⭐ | Feature | 🪟 Win11 | 🐧 Linux | 🚀 Impossible OS |`
    - Keep cells short: status emoji + max 5 words per cell. Rows under ~100 chars.
    - Mark each row `💎` (parity) or `⭐` (exclusive/superior).
    - Status: ✅ Done, ⚠️ Partial, ⬜ Planned (with `§N` reference), ❌ Not applicable.
    - Populate from research in step 2, not guesswork.
    - Add a summary paragraph after the table noting competitive position at each milestone.
    - Add `<!-- Sources: URL1, URL2 -->` comment after the summary for research traceability.

11. **Add the Unit Tests section (mandatory).**
    - Place after the last numbered implementation section, before Verification.
    - Required structure:
      - `> Wire into test_runner_init() via test_register_<feature>() -- register in src/kernel/test/test_runner.c.`
      - `- [ ] Create src/kernel/test/test_<feature>.c with:` followed by concrete test case sub-bullets.
      - Each test case: specific function call + expected return value or observable state. No vague "test that X works."
      - `- [ ] Register in test_runner_init(): test_register_<feature>()`
      - `- [ ] Commit: "test: add <feature> test suite"`
    - Derive test cases from the implementation sections' deliverables.
    - **Check if a new test category is needed.** If the subsystem doesn't fit existing `TEST_CAT_*` categories, note that a new `TEST_CAT_<NAME>` enum entry, `cat_names[]`/`cat_labels[]` entries, and `make test-<name>` Makefile target are needed. Reference the `implement-unit-tests` skill for the full procedure.

12. **Add the Verification section.**
    - Concrete platform-specific verification items, not vague "build and test."
    - Include: `bash scripts/build.sh clean` -> `=== BUILD OK ===`.
    - Include runtime verification with **specific observable outcomes**: exact serial log strings to grep for, specific function calls that should return specific values, specific POST16 code sequences.
    - Include platform list: QEMU WHPX (2 CPUs), QEMU TCG, VirtualBox, bare metal.
    - For hardware-touching TODOs: "Verify on bare metal -- VM behavior differs."

13. **Do NOT add model tags to section headings.**
    - No `[Opus]` or `[Sonnet]` postfixes -- model selection is handled by the harness, not the TODO.

14. **Update indexes in the same task.**
    - Update the domain `INDEX.md`.
    - Update `todo/TODO-00-INDEX.md` only when the new TODO changes root-visible scope.

## Guardrails

- Do not implement code or reconcile checkbox truth against the codebase.
- Do not add giant mutable prompt blocks, formatting-only cleanup steps, or MCP-memory instructions.
- Keep tables compact and split wide planning content into bullets or short subsections.

## Additional Resources

- [todo-template.md](todo-template.md)
- [implementation-order.md](implementation-order.md)
