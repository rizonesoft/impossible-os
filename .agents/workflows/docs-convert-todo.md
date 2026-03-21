---
description: Convert a fully completed TODO file into documentation and a stub
---

# Convert TODO to Documentation

When **all items** in a TODO file are `[x]` completed, convert it to a proper documentation file and leave a stub in place.

## Prerequisites

- Every item in the TODO file must be `[x]` (fully complete)
- The feature must be verified working (`/docs-verify-todo` passed)
- If any items are `[ ]` or `[/]`, do NOT convert — finish the TODO first

## Steps

### 1. Read the TODO file completely

Note everything that must be preserved:
- Technical details (architecture, data structures, algorithms)
- Implementation decisions and rationale
- Hardware/protocol constraints
- API contracts and function signatures
- Cross-references (XREFs) to other TODOs
- Gotchas, edge cases, known limitations
- Commit references
- OS Comparison tables
- Verification steps and expected behavior

### 2. Verify against the codebase

Before writing the doc, **confirm the TODO content matches the actual implementation:**

- Use Srclight (`get_symbol`, `get_callers`, `get_callees`) to verify function signatures, struct layouts, and call chains described in the TODO
- Check that file paths mentioned in the TODO actually exist
- Confirm register values, constants, and hardware addresses match the source code
- If the TODO says "uses DMA" but the code uses PIO, the doc must reflect the code — not the TODO
- Note any discrepancies to flag in a "Deviations" section if needed

> The codebase is the source of truth. The TODO describes intent; the doc must describe reality.

### 3. Determine the documentation path

| TODO scope | Doc path |
|-----------|----------|
| Driver (AHCI, VirtIO, NIC) | `docs/architecture/drivers/<name>.md` |
| Filesystem (FAT32, IXFS, ext4) | `docs/architecture/filesystem/<name>.md` |
| Kernel subsystem (PMM, VMM, scheduler) | `docs/architecture/kernel/<name>.md` |
| Desktop component | `docs/architecture/desktop/<name>.md` |
| Networking | `docs/architecture/net/<name>.md` |
| Boot/firmware | `docs/architecture/boot/<name>.md` |

Create subdirectories as needed.

### 4. Write the documentation file

Transform the TODO content into **rich, well-formatted technical documentation**.

#### Content rules — what to REMOVE vs KEEP

**REMOVE these (TODO-specific scaffolding):**
- `**Prompt:**` instruction blocks
- `- [ ]` / `- [x]` / `- [/]` checkbox formatting
- `- [ ] Commit: "scope: msg"` commit instructions
- Status emojis on section headers (⏳ 🔄 ✅)
- `> [!IMPORTANT] → XREF:` blocks (convert to regular cross-references)
- Priority markers (🔴 🟠 🟡 🟢)
- Status tracking in feature tables (`⬜ planned`, `⏳ in progress`) — docs describe what IS, not what's planned

**KEEP and restructure into documentation:**
- All technical content, architecture diagrams, data structures
- Implementation details, code examples, register layouts
- Design decisions and rationale ("why we chose X over Y")
- Hardware constraints and protocol requirements
- API surface (function signatures, parameters, return values)
- Error handling behavior
- OS Comparison tables (these show competitive positioning)
- Known gotchas, edge cases, limitations
- Performance notes
- Commit references (as footnotes or history section)
- Cross-references (convert XREFs to markdown links: `[AHCI Driver](../drivers/ahci.md)`)

#### Formatting — make it RICH

The documentation must be visually clear and information-dense:

- **Mermaid diagrams** for architecture, data flow, state machines, boot sequences
- **Tables** for register layouts, field descriptions, flag values, API parameters, comparisons
- **Aligned table columns** — pad cells so columns line up in raw markdown for readability
- **Code blocks** with language tags for struct definitions, function signatures, example usage
- **Admonition blocks** (`> [!WARNING]`, `> [!CAUTION]`, `> [!NOTE]`) for gotchas and critical constraints
- **Hierarchical headers** (H2 for major sections, H3 for subsections) — never skip levels
- **Cross-reference links** to related docs, specs, and source files
- **Inline code** for function names, constants, register names, file paths

**Example — data flow diagram:**

```mermaid
graph LR
    A[Application] --> B[VFS Layer]
    B --> C[FAT32 Driver]
    C --> D[Block Device]
    D --> E[AHCI HBA]
    E --> F[Physical Disk]
```

**Example — register layout table:**

| Bits | Field | Description |
|------|-------|-------------|
| 31:28 | Command | AHCI command type |
| 27:16 | PRDTL | PRD table length |
| 15:0 | Flags | Control flags |

#### Document structure

```markdown
# <Component Name>

> One-line summary of what this component does.

## Overview

High-level description, purpose, where it fits in the system.
Include a mermaid diagram showing where this component sits in the stack.

## Architecture

Data structures, flow diagrams, register layouts.
Use mermaid for state machines, tables for register/field layouts.

## Implementation

Key functions, code patterns, algorithms.
Include function signatures as code blocks.

## API

Public interface — functions, parameters, return values.
Use tables for parameter descriptions.

## Error Handling

How errors are detected, reported, and recovered from.

## Gotchas

Edge cases, footguns, hard-won lessons with commit refs.
Use `> [!CAUTION]` blocks for the most critical ones.

## OS Comparison

(if the TODO had one — preserve as-is with emoji table)

## References

- Spec: [AHCI 1.3.1](../specs/storage/ahci-1.3.1.md)
- Related: [VirtIO Driver](../drivers/virtio.md)
- Source: `src/kernel/drivers/ahci.c`
```

### 5. Replace the TODO file with a stub

Replace the original TODO file content (do NOT delete the file) with:

```markdown
# TODO-NNN.NN — <Title> ✅

**Status:** Completed — YYYY-MM-DD
**Documentation:** [component-name.md](../../docs/architecture/<path>/component-name.md)
**Commits:** `abc1234`, `def5678`, ...

## Gotchas

- Critical gotcha 1 (the kind that causes subtle bugs)
- Critical gotcha 2
- ...

## Cross-References

- Depends on: TODO-NNN §N.N (link if still active)
- Depended on by: TODO-NNN §N.N
```

**What goes in the stub gotchas:** Only the most critical things — memory allocation footguns, hardware constraints that cause silent failures, non-obvious API contracts. These are the "landmines" someone might hit if they modify this code without reading the full docs.

### 6. Update the TODO index

Open `todo/TODO-000-INDEX.md` and mark the converted TODO entry with ✅ and a link to the docs:

```markdown
- [x] TODO-040.02 — AHCI Driver → [docs](docs/architecture/drivers/ahci.md) ✅
```

### 7. Scan existing docs for consistency

After creating the new doc, scan the `docs/` folder for issues:

- **Stale references:** Search all docs for links to the old TODO file — update them to point to the new doc
- **Broken links:** Check that all `[text](path)` links in the new doc resolve to existing files
- **Cross-doc consistency:** If other docs reference the same component, verify they don't contradict the new doc
- **Spec references:** Verify `docs/specs/` links are correct (not old `specs/` paths)

```bash
# Quick check for stale references to old TODO paths
grep -r "TODO-NNN" docs/ --include="*.md"
```

### 8. Commit

```bash
git add -A && git commit -m "docs: convert TODO-NNN to documentation"
```

## Final Checklist

Before committing, verify:
- [ ] All technical content from the TODO is in the doc (nothing lost)
- [ ] No prompts, checkboxes, or commit instructions remain in the doc
- [ ] Doc matches the actual codebase (verified via Srclight)
- [ ] Rich formatting: mermaid diagrams, tables, code blocks, admonitions
- [ ] XREFs converted to markdown links
- [ ] Stub has status, doc link, commits, and critical gotchas
- [ ] TODO index updated
- [ ] No stale references in other docs
- [ ] Build still passes (`bash scripts/build.sh`)
