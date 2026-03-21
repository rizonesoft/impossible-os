---
description: Create a new spec document from research and fact-check it against authoritative sources
---

# Create a Spec Document

Research, write, and fact-check a technical specification document for a hardware component,
protocol, filesystem, or standard that Impossible OS needs to implement.

## When to Run

- Before creating a TODO for a new driver, filesystem, or protocol
- When no spec exists under `specs/` for a technology the project needs
- When the user requests a new spec document

## Input

- **Topic:** The technology to document (e.g., "NVMe 1.4", "xHCI USB 3.0", "Wayland protocol")
- **Category:** Where it belongs under `specs/` (e.g., `storage/controllers/`, `hardware/bus/`)

## Steps

### 1. Determine file location

Place specs using the existing directory structure:

| Category              | Path                             | Examples                          |
| --------------------- | -------------------------------- | --------------------------------- |
| Storage controllers   | `specs/storage/controllers/`     | AHCI, VirtIO, NVMe, xHCI         |
| Partitioning          | `specs/storage/partitioning/`    | GPT, MBR                         |
| Filesystems           | `specs/storage/filesystems/`     | FAT32, ext4, NTFS, Btrfs         |
| CPU architecture      | `specs/hardware/cpu/`            | Intel SDM, AMD64 Zen             |
| Bus protocols         | `specs/hardware/bus/`            | PCI, USB, PCIe                   |
| Firmware              | `specs/hardware/firmware/`       | UEFI, ACPI                       |
| Interrupts            | `specs/hardware/interrupts/`     | APIC, MSI-X                      |
| Hypervisors (Hyper-V) | `specs/hypervisors/hyper-v/`     | VMBus, StorVSC, synthetic devices|
| Networking            | `specs/networking/`              | TCP/IP, DNS, TLS (create if new) |

File naming: `<name>-<version>.md` (e.g., `nvme-1.4.md`, `xhci-1.2.md`) or just `<name>.md`
if unversioned (e.g., `btrfs.md`).

> [!IMPORTANT]
> Check if a spec already exists before creating a new one:
> ```bash
> ls specs/**/*.md
> ```
> If one exists, use `/specs-fact-check` instead of this workflow.

### 2. Research the technology

Gather information from authoritative sources **before writing anything**.

#### 2a. Identify authoritative sources

| Source type              | Examples                                                          |
| ------------------------ | ----------------------------------------------------------------- |
| Official specification   | OASIS VirtIO 1.2, Intel AHCI 1.3.1 PDF, Microsoft FAT32 spec     |
| Standards body documents | UEFI Forum, PCI-SIG, USB-IF, IETF RFCs                           |
| Kernel source code       | Linux kernel headers/drivers (definitive for on-wire formats)     |
| Vendor documentation     | Microsoft TLFS, Intel SDM, AMD APM                                |
| Community references     | OSDev Wiki, kernel.org docs, man pages                            |

Use `search_web` and `read_url_content` to pull from these sources.
Cross-reference at least **two independent sources** for every critical claim.

#### 2b. Research checklist

Gather ALL of the following that apply to the technology:

- [ ] **Version history** — release dates, key additions per version
- [ ] **Device/protocol identification** — IDs, signatures, magic numbers
- [ ] **Register layouts** — offsets, sizes, bit fields, reset values
- [ ] **Data structures** — on-disk or in-memory struct layouts with field sizes
- [ ] **Configuration space** — config fields, feature gates, access rules
- [ ] **Initialization sequence** — step-by-step protocol with ordering constraints
- [ ] **Feature negotiation** — feature bits, capability flags, optional features
- [ ] **I/O request types** — command opcodes, request formats, response formats
- [ ] **Error handling** — error codes, recovery procedures, reset sequences
- [ ] **Interrupt mechanisms** — INTx, MSI, MSI-X, polling alternatives
- [ ] **Memory requirements** — alignment, contiguity, DMA constraints
- [ ] **QEMU testing flags** — exact `-device` and `-drive` options for testing

### 3. Write the spec document

Use the established spec document format. The document should be a **comprehensive
technical reference** — not a tutorial, not a summary. It must contain enough detail
for an OS developer to implement a driver from scratch.

#### 3a. Required sections

Every spec document must include these sections (omit only if truly N/A):

```markdown
# <Name> <Version> — Technical Specification for OS Implementation

## Overview and Architectural Context
Brief description of what the technology does, where it fits in the stack,
and why an OS developer needs it. Include historical context.

### Version History and Compatibility
Table: version, date, key additions.

---

## <Transport / Identification Section>
How to detect and identify the device/protocol.
Vendor IDs, device IDs, signatures, capability discovery.

## <Core Configuration / Layout Section>
Register layouts, config space fields, data structures.
ALL offsets, sizes, bit fields, and access rules.

## <Initialization / Protocol Sequence Section>
Step-by-step initialization with exact ordering.
Status registers, handshake protocol, feature negotiation.

## <Feature Negotiation Section> (if applicable)
Complete feature bit table with bit numbers, names, descriptions.
Separate tables for transport features vs device-specific features.
Note which features are mandatory vs optional.

## <I/O / Operation Section>
Request types, command formats, response formats.
Data transfer mechanisms, descriptor layouts.
Include code-style struct definitions.

## <Error Handling Section>
Error codes, recovery procedures, reset sequences.
Timeout values, retry policies.

## <Interrupt / Notification Section> (if applicable)
INTx, MSI-X configuration, polling fallback.

## QEMU Testing Configuration
Exact QEMU command lines for testing each feature.
Separate examples for basic, multi-queue, special features.

## Implementation Priorities for Impossible OS
Priority table (🔴 P0 through 🔵 P4) specific to our OS.
Reference current codebase state if a partial implementation exists.
```

#### 3b. Formatting rules

Follow these formatting conventions exactly:

| Rule                    | Requirement                                                   |
| ----------------------- | ------------------------------------------------------------- |
| Hex values              | `0x1234` format (not `1234h`, not `0X1234`)                   |
| Register/field names    | Inline code: `` `GHC.IE` ``, `` `capacity` ``                |
| Struct definitions      | C code blocks with `uint8_t`/`uint16_t`/`uint32_t`/`uint64_t`|
| Tables                  | Aligned columns (pad cells to widest entry)                   |
| Code blocks             | Language-tagged (` ```c `, ` ```bash `, not bare ` ``` `)    |
| Admonitions             | `> [!NOTE]`, `> [!WARNING]`, `> [!CAUTION]`, `> [!IMPORTANT]`|
| Headers                 | H1 title, H2 major sections, H3 subsections — no skipped levels|
| Line length             | ≤ 120 characters                                              |
| Section separators      | `---` between major sections                                  |
| Endianness              | Always state endianness for multi-byte fields                 |
| Bit numbering           | MSB:LSB notation (e.g., "bits 31:28")                         |

#### 3c. Quality standards

- **Every register offset must have a source** — no guessing
- **Every bit field must specify the exact bit range** — not just "flags field"
- **Every struct must list field sizes in bytes** — uint8/16/32/64
- **Every constant must use the official name** from the authoritative spec
- **Every protocol sequence must specify the exact ordering constraints**
- **Feature-gated fields must be marked** with their feature gate
- Use `> [!CAUTION]` for **non-obvious hardware constraints** that cause real bugs
- Use `> [!IMPORTANT]` for **critical implementation notes** (e.g., capacity is in 512B sectors)
- Use `> [!NOTE]` for **helpful context** that aids understanding

### 4. Fact-check the document

After writing, immediately run the fact-checking procedure from `/specs-fact-check`.
This is mandatory — never commit an unchecked spec.

#### 4a. Verify every technical claim

Use `search_web` and `read_url_content` to verify against the authoritative source:

- [ ] All register offsets and bit fields match the official spec
- [ ] All constants and magic numbers are correct
- [ ] All struct layouts match on-disk/in-memory format exactly
- [ ] All protocol sequences are in the correct order
- [ ] All feature bits have correct bit numbers
- [ ] All QEMU flags are valid and tested

#### 4b. Check internal consistency

- [ ] Cross-references within the document resolve correctly
- [ ] Terminology is consistent throughout
- [ ] Units are consistent (bytes vs KiB vs sectors)
- [ ] Feature gates in config tables match the feature bit table
- [ ] Struct field offsets add up correctly (no gaps or overlaps)

#### 4c. Align all tables

Pad table cells so columns line up in raw markdown:

**Before:**
```markdown
| Field | Offset | Size | Description |
|---|---|---|---|
| Signature | 0x00 | 4 | HBA signature |
```

**After:**
```markdown
| Field     | Offset | Size | Description    |
| --------- | ------ | ---- | -------------- |
| Signature | 0x00   | 4    | HBA signature  |
```

### 5. Update the specs index

Add the new spec to `specs/index.md` in the correct category section.
Follow the existing table format with Document and Topics columns:

```markdown
| [Spec Name](path/to/spec.md) | Brief topic list |
```

Keep entries in alphabetical order within each category.

### 6. Log changes and commit

Create a summary of the spec for the commit message:

```bash
git add -A && git commit -m "specs: add <spec-name>

- <Topic> specification for OS implementation
- Covers: <list key areas>
- Verified against <authoritative source>
- N tables, N struct definitions, N register layouts"
```

## Checklist

Before committing, verify:
- [ ] File is in the correct `specs/` subdirectory
- [ ] Filename follows `<name>-<version>.md` convention
- [ ] Document has all required sections (overview, identification, config, init, I/O, errors, QEMU, priorities)
- [ ] All register offsets verified against authoritative source
- [ ] All constants and magic numbers verified
- [ ] All struct layouts verified (field sizes, order, alignment)
- [ ] All feature bits have correct bit numbers and names
- [ ] All protocol sequences verified for correct ordering
- [ ] All tables are column-aligned
- [ ] Code blocks have language tags
- [ ] Hex values use `0x` format consistently
- [ ] Headers follow proper hierarchy
- [ ] Line length ≤ 120 characters
- [ ] Admonitions used correctly for hardware gotchas
- [ ] QEMU testing commands are valid
- [ ] Implementation priorities table included
- [ ] `specs/index.md` updated with the new entry
- [ ] Fact-checked against at least two independent sources
