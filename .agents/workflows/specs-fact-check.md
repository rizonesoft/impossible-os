---
description: Fact-check a specs document, align table columns, and fix issues
---

> **Reference document.** `.cursor/rules/` and `.cursor/skills/` are the single source of truth for AI guidance.
> This workflow is not yet migrated to a Cursor skill. It is kept here as a reference until a `specs-fact-check` skill is created.

# Fact-Check Specs Document

Verify a specification document against authoritative external sources and internal consistency. Fix errors, align tables, and ensure the document is accurate and well-formatted.

## When to Run

- After creating or importing a new spec document
- When a spec is referenced by a TODO or implementation doc and accuracy matters
- Periodically as a quality check on reference material

## Input

A single spec file path, e.g., `specs/storage/controllers/ahci-1.3.1.md`

## Steps

### 1. Identify the spec's authoritative source

Determine what external standard this spec documents:

| Spec type    | Authoritative source                                |
|--------------|-----------------------------------------------------|
| AHCI         | Intel AHCI 1.3.1 specification PDF                  |
| VirtIO       | OASIS VirtIO 1.2 specification                      |
| FAT32        | Microsoft FAT32 File System Specification            |
| UEFI         | UEFI Forum UEFI Specification 2.10                  |
| ACPI         | UEFI Forum ACPI Specification 6.5                   |
| PCI          | PCI-SIG PCI Local Bus Specification 3.0              |
| ext4         | kernel.org ext4 documentation + on-disk format wiki  |
| NTFS         | Microsoft NTFS documentation + ntfsdoc.pdf           |
| Hyper-V      | Microsoft Hyper-V TLFS + Linux hvlite sources        |
| GPT/MBR      | UEFI Specification Chapter 5 + Microsoft docs        |

Use `search_web` to pull current spec data if needed. Do NOT guess — verify against the official source.

### 2. Fact-check technical claims

For every technical claim in the document, verify accuracy:

#### Register layouts and bit fields
- **Verify offsets:** Register addresses must match the official spec exactly
- **Verify bit ranges:** Field positions (e.g., "bits 31:28") must be correct
- **Verify reset values:** Default/reset values must match the spec
- **Verify field names:** Use the exact names from the official spec (not paraphrased)

#### Constants and magic numbers
- **Signature values:** e.g., AHCI `0x00010301`, FAT32 BPB `0xAA55`
- **Size values:** structure sizes, alignment requirements
- **Capability flags:** feature bits, status codes, command opcodes
- **Verify against the official spec, not memory**

#### Data structures
- **Field order:** must match on-disk or in-memory layout exactly
- **Field sizes:** `uint8_t` vs `uint16_t` vs `uint32_t` must be correct
- **Padding and alignment:** verify struct packing matches the spec
- **Endianness:** note if fields are little-endian vs big-endian

#### Protocol sequences
- **Handshake order:** initialization steps must be in the correct order
- **Timeout values:** must match the spec's required minimums/maximums
- **Error conditions:** must list all error codes the spec defines

### 3. Verify against authoritative web sources

Use `search_web` and `read_url_content` to verify claims against the official specification or trusted references
(OSDev Wiki, kernel.org, Microsoft docs, Intel/AMD manuals):

- Search for the specific register, constant, or protocol detail being verified
- Compare the spec doc's claim with at least one authoritative source
- For each discrepancy, fix the spec doc to match the authoritative source
- Add a `> [!NOTE]` block citing the source when correcting a non-obvious error

> **The official spec is the source of truth.** These documents guide our implementation — never
> fact-check them against local code. The code should conform to the spec, not the other way around.

### 4. Check internal consistency

Within the spec document itself:
- **Cross-references:** Do section links (`#section-name`) resolve correctly?
- **Table of contents:** If the doc has one, does it match the actual headings?
- **Forward references:** Does the doc reference structs/registers before defining them?
- **Terminology:** Is naming consistent? (e.g., don't mix "HBA" and "host adapter")
- **Units:** Are sizes consistently in bytes, KiB, or MiB? Don't mix.

### 5. Align all table columns

Pad table cells so columns line up in raw markdown:

**Before:**
```markdown
| Field | Offset | Size | Description |
|---|---|---|---|
| Signature | 0x00 | 4 | HBA signature |
| Version | 0x04 | 4 | AHCI version |
```

**After:**
```markdown
| Field     | Offset | Size | Description    |
| --------- | ------ | ---- | -------------- |
| Signature | 0x00   | 4    | HBA signature  |
| Version   | 0x04   | 4    | AHCI version   |
```

Rules:
- Pad all cells to the width of the widest cell in that column
- Use consistent separator row style (`| --- |` padded to column width)
- Align numeric values (hex addresses, sizes) for visual scanning
- Keep table width ≤ 120 characters — split wide tables if needed

### 6. Fix formatting issues

- **Code blocks:** Ensure language tags are present (` ```c `, ` ```asm `, not bare ` ``` `)
- **Admonitions:** Use `> [!NOTE]`, `> [!WARNING]`, `> [!CAUTION]` correctly
- **Headers:** H1 for title, H2 for major sections, H3 for subsections — no skipped levels
- **Hex values:** Use consistent format (`0x1234` not `1234h` or `0X1234`)
- **Register names:** Use inline code (`` `GHC.IE` ``) not plain text
- **Mermaid diagrams:** Verify they render correctly (no syntax errors)
- **Line length:** Keep under 120 characters (split long lines)

### 7. Log changes

After all fixes, create a summary of what was changed:

```markdown
## Fact-Check Summary

- **Errors fixed:** N
- **Tables aligned:** N
- **Implementation deviations noted:** N
- **Key changes:**
  - Fixed `CAP.NCS` bit range (was 12:8, correct is 12:8 — confirmed)
  - Added missing `PxSERR` register fields
  - Aligned 5 tables
  - Added WARNING for unimplemented NCQ
```

This summary goes in the commit message, not in the spec document itself.

### 8. Commit

```bash
git add -A && git commit -m "specs: fact-check <spec-name>

- Verified against <authoritative source>
- Fixed N errors, aligned N tables
- <key changes summary>"
```

## Checklist

Before committing, verify:
- [ ] All register offsets and bit fields match the authoritative spec
- [ ] All constants and magic numbers are correct
- [ ] Struct layouts match on-disk/in-memory format
- [ ] Protocol sequences are in the correct order
- [ ] All technical claims verified against authoritative sources
- [ ] All table columns are aligned
- [ ] Code blocks have language tags
- [ ] Hex values use consistent `0x` format
- [ ] Headers follow proper hierarchy (no skipped levels)
- [ ] Internal cross-references resolve correctly
- [ ] Line length ≤ 120 characters
