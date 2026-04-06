---
name: create-todo
description: Create lean project TODO files under todo/, choose the correct domain and local number, build the canonical Implementation Order table, wire XREFs, and update indexes. Use when the user asks to create, author, scaffold, or derive a new TODO roadmap from a feature, spec, or subsystem.
---

# Create TODO

## Use This Skill When

- The user wants a new TODO file under `todo/`.
- A feature, subsystem, or spec needs a tracked execution roadmap.
- A stale draft needs to be rewritten into the current lean TODO format.

## Workflow

1. **Find the canonical home.**
   - Read `todo/TODO-00-INDEX.md`, the target domain `INDEX.md`, and adjacent TODOs.
   - Prefer one canonical TODO home and cross-link instead of duplicating scope.

2. **Research the domain (critical -- do not skip).**
   - If a spec exists, treat it as the primary structural input.
   - Use Grep/Glob to discover existing implementations, related symbols, and prior art.
   - Use `git log` to discover recent work that may already cover planned scope.
   - **Win11/Linux competitive research:** Run at least 6 web searches (3 Win11, 3 Linux) to build a feature inventory for the OS Comparison table. Queries: `"Windows 11 <topic> internals"`, `"Linux kernel <topic>"`, `"modern OS <topic> best practices"`. Follow at least 2 links with WebFetch for detailed feature descriptions. This is what makes the OS Comparison table accurate instead of guesswork.
   - Build a feature inventory: for each feature, record Win11 status, Linux status, and whether it's parity (both have it) or competitive edge (neither does it well).

3. **Cross-TODO overlap scan.**
   - Use Grep to search ALL domain folders for keywords from the planned section titles and deliverables.
   - For each match, classify: SUPERSEDES, COMPLEMENT, CONFLICT, or FOUNDATION.
   - Add bidirectional `-> XREF:` links for every real dependency or overlap.
   - Prefer one canonical TODO home -- do not duplicate scope.

4. **Choose the next local filename.**
   - Follow the live domain naming pattern such as `todo/00-infrastructure/TODO-03-short-name.md`.
   - Do not revive legacy `TODO-NNN.NN-*` naming from `todo-old/`.

5. **Draft the TODO in the current lean format.**
   - Default to a leaf TODO.
   - Create a parent TODO only when the topic truly needs multiple child files or shared verification.
   - Use the template in [todo-template.md](todo-template.md) as the structural starting point.

6. **Build sections with full structure.**
   - Use flat numbered sections (`## 1. Title`, `## 2. Title`) -- not sub-sections (`### 1.1`).
   - Every section MUST include:
     - Concrete checklist items (`- [ ]`) referencing specific functions, types, or files.
     - A `- [ ] Commit: "scope: short description"` line as the last checklist item.
     - A `**Test checkpoint:**` block with concrete pass/fail criteria: exact serial strings, POST16 codes, expected behavior, and platforms (QEMU WHPX + TCG; bare metal).
   - For sections touching boot-path, interrupts, page tables, GDT/TSS, or timers:
     - Add `POST16(0xDDNN)` entry/exit codes. Check `boot_init.h` for conflicts before assigning.
     - Add a `**Regression risk:**` note identifying what could break and rollback strategy.

7. **Set up the Implementation Order table.**
   - Use `Implementation Order`, not `Phase-by-Phase`.
   - Mark every row `💎` (parity) or `⭐` (exclusive).
   - Use compact `§N` notation for internal dependencies, `TNN §N` for same-domain, `DNN TNN §N` for cross-domain. Never bare numbers.
   - Every `## N.` body section MUST have a corresponding table row, and vice versa.
   - Cross-TODO references MUST include specific section numbers (`T11 §1,§3`). Never bare TODO numbers.
   - Add dependencies, overlap notes, handoffs, and `-> XREF:` links while creating the file.

8. **Add the OS Comparison table (mandatory).**
   - Place before Unit Tests / Verification sections.
   - Header: `| ⭐ | Feature | 🪟 Win11 | 🐧 Linux | 🚀 Impossible OS |`
   - Keep cells short: status emoji + max 5 words per cell. Rows under ~100 chars.
   - Mark each row `💎` (parity) or `⭐` (exclusive/superior).
   - Status: ✅ Done, ⚠️ Partial, ⬜ Planned (with `§N` reference), ❌ Not applicable.
   - Populate from research in step 2, not guesswork. Cite features found during web searches.
   - Add a summary paragraph after the table noting competitive position at each milestone.

9. **Add the Unit Tests section (mandatory).**
   - Place after the last numbered implementation section, before Verification.
   - Required structure:
     - `> Wire into test_runner_init() via test_register_<feature>() -- register in src/kernel/test/test_runner.c.`
     - `- [ ] Create src/kernel/test/test_<feature>.c with:` followed by concrete test case sub-bullets.
     - Each test case: specific function call + expected return value or observable state. No vague "test that X works."
     - `- [ ] Register in test_runner_init(): test_register_<feature>()`
     - `- [ ] Commit: "test: add <feature> test suite"`
   - Derive test cases from the implementation sections' deliverables.

10. **Add the Verification section.**
    - Concrete platform-specific verification items, not vague "build and test."
    - Include: `bash scripts/build.sh clean` -> `=== BUILD OK ===`.
    - Include runtime verification on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
    - Reference specific serial log strings, POST16 codes, and user-visible behavior.
    - For hardware-touching TODOs: "Verify on bare metal -- VM behavior differs."

11. **Do NOT add model tags to section headings.**
    - No `[Opus]` or `[Sonnet]` postfixes -- model selection is handled by the harness, not the TODO.

12. **Update indexes in the same task.**
    - Update the domain `INDEX.md`.
    - Update `todo/TODO-00-INDEX.md` only when the new TODO changes root-visible scope.

## Guardrails

- Do not implement code or reconcile checkbox truth against the codebase.
- Do not add giant mutable prompt blocks, formatting-only cleanup steps, or MCP-memory instructions.
- Keep tables compact and split wide planning content into bullets or short subsections.

## Additional Resources

- [todo-template.md](todo-template.md)
- [implementation-order.md](implementation-order.md)
