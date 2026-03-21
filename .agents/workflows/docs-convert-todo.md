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

Choose the most appropriate location under `docs/` based on the content — not just the TODO category. The doc should land where a developer would naturally look for it.

**Domain directories (create subdirectories as needed):**

| Directory                 | Content Type                                              |
|---------------------------|-----------------------------------------------------------|
| `docs/kernel/`            | Boot chain, memory (PMM/VMM/heap), scheduler, IPC         |
| `docs/storage/`           | Controllers (AHCI, VirtIO), partitioning (GPT), filesystems (FAT32, ext4) |
| `docs/networking/`        | Network drivers (RTL8139), protocols (ARP, IPv4, DHCP)    |
| `docs/graphics/`          | 2D rendering, compositing, desktop shell                  |
| `docs/hypervisors/`       | Hyper-V, VirtualBox integration                           |
| `docs/hardware/`          | CPU architecture, PCI bus, ACPI/UEFI firmware, APIC       |
| `docs/infrastructure/`    | Build system, CI/CD, tooling, development environment     |
| `docs/getting-started/`   | Setup guides and tutorials                                |

**Examples:**

| TODO                       | Doc path                                            | Rationale                          |
|----------------------------|-----------------------------------------------------|------------------------------------|
| AHCI driver                | `docs/storage/controllers/ahci.md`                  | Storage controller implementation  |
| FAT32 filesystem           | `docs/storage/filesystems/fat32.md`                 | Filesystem driver                  |
| Build system tooling       | `docs/infrastructure/development-tooling.md`        | Dev tooling, not OS internals      |
| GitHub CI/CD setup         | `docs/infrastructure/github-setup.md`               | Repo infrastructure                |
| UEFI bootloader            | `docs/kernel/boot/uefi-bootloader.md`               | Kernel boot chain                  |
| VMBus driver               | `docs/hypervisors/hyper-v/vmbus.md`                 | Hypervisor integration             |
| RTL8139 NIC                | `docs/networking/drivers/rtl8139.md`                | Network driver                     |

> Match the domain, not the TODO category. The doc should land where a developer would naturally look for it.

### 3b. Check for topic duplication (MANDATORY)

Before creating the doc, check if the topic is already covered:

1. **Read the `index.md`** in the target category folder (e.g., `docs/infrastructure/index.md`)
2. **Scan the "Owned Topics" column** — does any existing doc already own topics you're about to write about?
3. **If overlap found:**
   - If the overlap is small (a few paragraphs), **link** to the existing doc's section instead of re-explaining
   - If the overlap is large (entire sections), **merge** into the existing doc and update its index.md entry
   - If the topics are genuinely distinct but live in the same area, ensure clear **topic boundaries** between the docs
4. **Register the new doc** in the category's `index.md` with its owned topics
5. **Update `docs/index.md`** if a new category was created

> The rule is simple: each concept is explained in exactly one place. Everything else links to it.

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

### 9. Run `/docs-validate`

Always run the `/docs-validate` workflow after committing. This catches broken links, stale references, and structural violations introduced by the conversion.

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
- [ ] `/docs-validate` passed (no broken links or structural violations)
- [ ] Build still passes (`bash scripts/build.sh`)
