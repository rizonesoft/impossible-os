---
description: Convert a fully completed TODO file into documentation and a stub
---

# Convert TODO to Documentation

When **all items** in a TODO file are `[x]` completed, convert it to a proper documentation file and leave a stub in place.

## Prerequisites

- Every item in the TODO file must be `[x]` (fully complete)
- The feature must be verified working (`/verify-todo` passed)
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

### 2. Determine the documentation path

| TODO scope | Doc path |
|-----------|----------|
| Driver (AHCI, VirtIO, NIC) | `docs/architecture/drivers/<name>.md` |
| Filesystem (FAT32, IXFS, ext4) | `docs/architecture/filesystem/<name>.md` |
| Kernel subsystem (PMM, VMM, scheduler) | `docs/architecture/kernel/<name>.md` |
| Desktop component | `docs/architecture/desktop/<name>.md` |
| Networking | `docs/architecture/net/<name>.md` |
| Boot/firmware | `docs/architecture/boot/<name>.md` |

Create subdirectories as needed.

### 3. Write the documentation file

Transform the TODO content into proper technical documentation. The doc MUST contain **all technical information** from the TODO — nothing is lost except:

**REMOVE these (TODO-specific scaffolding):**
- `**Prompt:**` instruction blocks
- `- [ ]` / `- [x]` / `- [/]` checkbox formatting
- `- [ ] Commit: "scope: msg"` commit instructions
- Status emojis on section headers (⏳ 🔄 ✅)
- `> [!IMPORTANT] → XREF:` blocks (convert to regular cross-references)
- Priority markers (🔴 🟠 🟡 🟢)

**KEEP and restructure these into documentation:**
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

**Document structure:**

```markdown
# <Component Name>

> One-line summary of what this component does.

## Overview

High-level description, purpose, where it fits in the system.

## Architecture

Data structures, flow diagrams, register layouts.

## Implementation

Key functions, code patterns, algorithms.

## API

Public interface — functions, parameters, return values.

## Gotchas

Edge cases, footguns, hard-won lessons with commit refs.

## OS Comparison

(if the TODO had one — preserve as-is)

## References

- Spec: [docs/specs/storage/ahci-1.3.1.md](...)
- Related: [VirtIO Driver](../drivers/virtio.md)
```

### 4. Replace the TODO file with a stub

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

### 5. Update the TODO index

Open `todo/TODO-000-INDEX.md` and mark the converted TODO entry with ✅ and a link to the docs:

```markdown
- [x] TODO-040.02 — AHCI Driver → [docs](docs/architecture/drivers/ahci.md) ✅
```

### 6. Update spec references

If the TODO referenced files in the old `specs/` directory, update paths to `docs/specs/`.

### 7. Commit

```bash
git add -A && git commit -m "docs: convert TODO-NNN to documentation"
```

## Checklist

Before committing, verify:
- [ ] All technical content from the TODO is in the doc
- [ ] No prompts, checkboxes, or commit instructions remain in the doc
- [ ] XREFs converted to markdown links
- [ ] Stub has status, doc link, commits, and critical gotchas
- [ ] TODO index updated
- [ ] Build still passes (`bash scripts/build.sh`)
