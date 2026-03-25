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

1. Find the canonical home.
   - Read `todo/TODO-00-INDEX.md`, the target domain `INDEX.md`, and adjacent TODOs.
   - Prefer one canonical TODO home and cross-link instead of duplicating scope.
2. Gather the primary inputs.
   - If a spec exists, treat it as the primary structural input.
   - Otherwise use current code, docs, and related TODOs to define scope.
3. Choose the next local filename.
   - Follow the live domain naming pattern such as `todo/00-infrastructure/TODO-03-short-name.md`.
   - Do not revive legacy `TODO-NNN.NN-*` naming from `todo-old/`.
4. Draft the TODO in the current lean format.
   - Default to a leaf TODO.
   - Create a parent TODO only when the topic truly needs multiple child files or shared verification.
5. Set up execution order at creation time.
   - Use `Implementation Order`, not `Phase-by-Phase`.
   - Add dependencies, overlap notes, handoffs, and `→ XREF:` links while creating the file.
6. Update indexes in the same task.
   - Update the domain `INDEX.md`.
   - Update `todo/TODO-00-INDEX.md` only when the new TODO changes root-visible scope.

## Guardrails

- Do not implement code or reconcile checkbox truth against the codebase.
- Do not add giant mutable prompt blocks, formatting-only cleanup steps, or MCP-memory instructions.
- Keep tables compact and split wide planning content into bullets or short subsections.

## Additional Resources

- [todo-template.md](todo-template.md)
- [implementation-order.md](implementation-order.md)
