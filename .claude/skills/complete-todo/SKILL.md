---
name: complete-todo
description: Convert a fully completed TODO file into polished project documentation, update indexes, handle XREF resolution, and remove the TODO. Use when all sections of a TODO are [x] and it's ready to graduate from planning to documentation.
---

# Complete TODO → Documentation

## When to Use

- All sections in a TODO's Implementation Order table are `[x]`
- All Unit Tests and Verification items are done
- The user says "complete TODO-XX" or "convert TODO-XX to docs"

## Workflow

### 1. Verify completeness

Read the TODO file. Confirm:
- Every Implementation Order row has `[x]` status
- Unit Tests section is fully `[x]` (or marked N/A)
- Verification section has PASS results
- No `[ ]` or `[/]` items remain (except manual/hardware checks)

If incomplete items remain, **stop and report** what's still open.

### 2. Check for existing doc

Search `docs/` for an existing document covering the same topic:
- Check the obvious subfolder (`docs/kernel/`, `docs/infrastructure/`, `docs/drivers/`, etc.)
- Search by keywords from the TODO title
- If found: **update the existing doc** with new information rather than creating a duplicate

### 3. Fact-check against the codebase

Before writing the doc, verify every claim against actual code:
- Function signatures — grep the headers, confirm they match
- File paths — glob to verify they exist
- Counts (suites, assertions, types) — run `scripts/test-coverage.sh` or grep
- Boot.conf settings — read `resources/boot/boot.conf`
- Config struct fields — read the actual header
- Build commands — verify they work

**Do NOT trust the TODO text blindly** — it was written as a plan. The code may have diverged.

### 4. Generate the documentation

Create a polished doc file following the quality standard of `docs/infrastructure/development-tooling.md`:

**Required sections:**
- **Title + one-line summary** (blockquote under `#` heading)
- **Overview** — what was built and why, with a **Mermaid architecture diagram**
- **Core sections** — each major feature/subsystem with:
  - Explanation of how it works
  - Tables for configuration, API surface, key constants
  - Code examples or command examples where helpful
  - Mermaid diagrams for data flow or architecture
- **Key Files** table — file path + purpose for every relevant source file
- **Gotchas** — hard-won lessons, common mistakes, things that look wrong but aren't
- **OS Comparison** table — carry over from TODO, update with actual state (replace planned markers with implemented descriptions)
- **References** — links to source directories, related docs, external specs

**Quality standards:**
- Every table should be properly aligned
- Mermaid diagrams for architecture, data flow, or process pipelines
- `> [!NOTE]`, `> [!WARNING]`, `> [!CAUTION]`, `> [!IMPORTANT]` callouts for gotchas
- No TODO planning artifacts (`[ ]`, `[x]`, "Commit:", "Test checkpoint:", XREFs)
- No speculative language ("planned", "will be", "deferred") — only what IS
- Concrete numbers from the codebase (not from the TODO's estimates)

### 5. Place in the correct docs subfolder

| TODO Domain | Docs Subfolder |
|---|---|
| `00-infrastructure/` | `docs/infrastructure/` |
| `01-boot-platform/` | `docs/boot/` |
| `02-kernel-core/` | `docs/kernel/` |
| `03-memory/` | `docs/kernel/memory/` |
| `04-drivers-hardware/` | `docs/drivers/` |
| `05-filesystem/` | `docs/filesystem/` |
| `06-networking/` | `docs/networking/` |
| `07-user-interface/` | `docs/desktop/` |
| Other | `docs/<appropriate>/` |

Use a descriptive filename: `kernel-test-framework.md`, not `TODO-03.md`.

### 6. Update ALL indexes

Three indexes must be updated:

1. **Docs folder `index.md`** — add the new doc to `docs/<subfolder>/index.md`
   - Add a row to the Documents table with the doc title and topic summary
   - If `index.md` doesn't exist in the subfolder, create one following the pattern from `docs/infrastructure/index.md`

2. **TODO domain `INDEX.md`** — move the entry from Active to Completed
   - Remove from "Active TODOs" section
   - Add to "Completed / Doc-converted" section with strikethrough and link to new doc

3. **`CLAUDE.md`** — update if the TODO was referenced there

### 7. Handle XREF resolution

Search all remaining TODO files for XREFs to the removed TODO:
```
grep -r "TODO-XX" todo/
```

For each reference:
- If it points to a specific section (`TODO-XX §3`): update to point to the doc file and section
- If it's a dependency that's now satisfied: add note "(completed, see docs/<path>)"
- If it's a back-XREF: update or remove

### 8. Commit

```
docs: convert TODO-XX to documentation — <topic name>

Graduated from todo/ to docs/: all sections complete, verified on
<platforms>. Removed TODO file, updated indexes and XREFs.
```

## Guardrails

- Do NOT convert incomplete TODOs — every section must be `[x]`
- Do NOT copy TODO text verbatim — rewrite as documentation
- Do NOT include planning artifacts in the doc (checkboxes, commit messages, test checkpoints)
- Do NOT remove the TODO without creating the doc first
- Do NOT break XREFs — update every reference before removing the source
- Always fact-check function signatures, file paths, and counts against the actual codebase
