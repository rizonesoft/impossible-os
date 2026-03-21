---
description: Create a new TODO file following the project's established format and structure, optionally driven by a spec document
---

# Create a TODO File

Create a comprehensive, properly structured TODO file for a new feature, driver, or subsystem.
Each TODO file is a complete implementation roadmap — not just a checklist.

---

## Creating a TODO from a Spec Document

When a spec document exists under `specs/`, it becomes the **primary input** for TODO creation.
This produces higher-quality, more accurate TODOs because the spec already contains register layouts,
feature bits, config offsets, struct definitions, protocol sequences, and testing commands.

### When to use this path

- A spec file already exists in `specs/` (e.g., `specs/storage/controllers/virtio-1.2.md`)
- The user explicitly says to create a TODO from a spec
- You're creating a TODO for a driver, filesystem, or protocol that has a corresponding spec

### How to map spec content → TODO content

| Spec Element                         | Maps To                                                                          |
| ------------------------------------ | -------------------------------------------------------------------------------- |
| Top-level sections (Transport, I/O)  | **TODO section groups** (`## N. Section Group Title`)                            |
| Feature bit tables                   | **TODO sections** — one per feature or group of related features                 |
| Config space offset tables           | **Checkbox items** with exact offsets, sizes, and field names                    |
| Struct definitions                   | **Checkbox items** referencing exact field names and types                       |
| Register bit-field tables            | **Checkbox items** with bit numbers, values, and flag names                      |
| Init/protocol sequence steps         | **Checkbox items** matching the spec's step numbering                            |
| Error/status code tables             | **Checkbox items** in an error-handling section                                  |
| QEMU testing commands                | **Checkbox items** at the end of each section (`- [ ] Test: ...`)                |
| Spec cautions/warnings               | **`> [!CAUTION]` / `> [!WARNING]` blocks** in the TODO header                   |
| Priority/recommendation tables       | **Priority Order table** — use the spec's own priority classifications           |
| Feature gate columns                 | **Dependency info** — features gated by other features form the dependency graph |

### Spec-driven workflow (replaces steps 2–6 for spec-based TODOs)

1. **Read the full spec** — `view_file` on the spec document under `specs/`
2. **Extract the structure** — identify all major sections, subsections, and tables
3. **Map sections to TODO groups** — each spec section becomes a `## N. Group` or `### N.M Section`
4. **Extract feature bits** — each feature bit (or closely related group) becomes a TODO section:
   - Section title = feature name (e.g., "Flush (Write Barriers)")
   - Prompt = reference the spec section, explain what to negotiate, what config to read, what API to expose
   - Checkboxes = one per config field, struct field, or protocol step from the spec
   - Include exact offsets, bit numbers, type constants, and struct layouts from the spec
5. **Build the dependency graph** — use the spec's own structure:
   - Transport/init sections → Phase 1 (always first)
   - Feature-gated configs → depend on the feature negotiation section
   - I/O request types → depend on the init + interrupt sections
   - Advanced features → depend on core I/O path
6. **Generate cross-references** — link the TODO to the spec:
   - `> [!IMPORTANT]` block with spec path and reference: `[Spec Name](file:///absolute/path/to/spec.md)`
   - Each section's Prompt should reference the relevant spec section number
7. **Extract QEMU testing flags** — copy verbatim from the spec's testing section into TIP blocks
8. **Continue with steps 7–11** of the standard workflow below (Priority Order, exclusive features, OS Comparison, index update, commit)

### Spec cross-reference rules

- Always link to the spec using `file:///` absolute paths
- Reference spec section numbers in prompts: "per VirtIO 1.2 §5.2.6"
- Quote exact hex values, bit numbers, and struct field names from the spec
- If the spec has a config space layout table, reproduce exact offsets in checkboxes
- If the spec has QEMU command examples, include them in the TODO's TIP block verbatim

### Example: Spec feature bit → TODO section

Given this spec table row:

```
| 6 | VIRTIO_BLK_F_BLK_SIZE | Block size in `blk_size` config field (may differ from 512) |
```

And this spec config entry:

```
| 0x14 | 4 | blk_size | F_BLK_SIZE | Logical block size in bytes |
```

Generate this TODO section:

```markdown
### N.M Block Size Negotiation

**Prompt:** Negotiate `VIRTIO_BLK_F_BLK_SIZE` (bit 6) per VirtIO 1.2 §5.2.3.
When offered, read the `blk_size` field from device config offset `0x14` (4 bytes, le32).
Store the value and use it instead of hardcoded 512 for all sector-to-byte conversions.
Default to 512 bytes if the feature is not offered. Log the negotiated block size.
After completing all items, mark every item as `[x]`, update this prompt to a
verification prompt, run `bash scripts/build.sh clean`, and commit as
`"driver: block size negotiation"`. After implementation, save gotchas to MCP memory.

- [ ] Negotiate `VIRTIO_BLK_F_BLK_SIZE` (bit 6)
- [ ] Read `blk_size` from device config offset `0x14` (le32)
- [ ] Store in device struct, default to 512 if not offered
- [ ] Use `blk_size` instead of hardcoded 512 in I/O path
- [ ] Log: `[Driver] Block size: %u bytes`
- [ ] Commit: `"driver: block size negotiation"`
```

## Prerequisites

- Identify the correct category folder under `todo/` (check `TODO-000-INDEX.md`)
- Determine the numbering: check existing TODO files in the target folder for the next available number
- Have the relevant spec document available (if applicable)
- Understand the existing codebase state for this component (via Srclight)

## Steps

### 1. Determine file location and numbering

Use the TODO folder structure from `AGENTS.md`:

| Prefix    | Layer                | Examples                                    |
| --------- | -------------------- | ------------------------------------------- |
| `000-`    | Infrastructure       | Build system, tooling, CI                   |
| `010-`    | Kernel Foundations   | Bootloader, threading, filesystem, registry |
| `060-`    | Hardware & Drivers   | Keyboard, mouse, power management           |
| `110-`    | GFX & UI Framework   | UI controls, theme, animation               |
| `160-`    | Desktop Shell        | Taskbar, start menu, boot splash            |
| `230-`    | Core Services        | Clipboard, search, security                 |
| `310-`    | Core Apps            | Terminal, file manager, notepad             |
| `380-`    | Multimedia           | Audio, paint                                |
| `400-`    | Networking           | Browser, FTP, SSH, email                    |
| `460-`    | Polish & Extras      | DPI, screensaver, widgets                   |
| `510-`    | Long-Term Stretch    | Win32, Linux compat, SDK                    |

File naming: `TODO-NNN.NN-ShortName.md` (e.g., `TODO-040.01-VirtIO.md`)

### 2. Research the component

Before writing the TODO:

1. **Read the relevant spec** (if one exists under `specs/`) — if a spec exists, follow the **"Creating a TODO from a Spec Document"** path above instead of this step
2. **Search the codebase** via Srclight (`search_symbols`, `get_symbol`, `symbols_in_file`) to understand current state
3. **Search the web** for authoritative sources on the technology
4. **Study competitor implementations** — how do Windows 11 and Linux handle this?
5. **Identify Impossible OS exclusive features** — what can we do better? (see §8 below)

### 3. Write the header

```markdown
# NNN.NN-ShortName — Full Descriptive Title

> **Goal:** 2–4 sentence description of what this TODO accomplishes. Include the current
> state of the component (what exists), what needs to change, and the end state. Reference
> the relevant spec by name and version. Mention any Impossible OS exclusive features.
```

### 4. Add critical notices (as needed)

Use GitHub admonition blocks for critical constraints that affect the entire TODO:

```markdown
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL DMA buffers. `kmalloc` is ONLY
> for small kernel structs (≤ 4 KB). See `rules.md` Known Gotchas.

> [!WARNING]
> **Hardware constraint:** Description of a hardware-level danger or requirement.

> [!IMPORTANT]
> **Spec Reference:** All section numbers, register offsets, and bit definitions reference the
> [Spec Name](file:///path/to/spec.md) (Standard Body, Year).
```

Only include notices that apply globally. Section-specific warnings go in the section's Notes.

### 5. Write the TODO Completion Roadmap

This is the most critical structural element — it shows how all sections relate.

#### 5a. Dependency Graph (Mermaid)

Create a mermaid `graph TD` showing:
- External prerequisites (spec, existing driver, other TODOs)
- All internal sections as nodes (use `§N.N Title` labels)
- Arrows showing strict dependencies
- Downstream consumers (which TODOs/filesystems depend on this work)

```markdown
### Dependency Graph

​```mermaid
graph TD
    SPEC["specs/path/to/spec.md<br/>Spec Name"]
    DRV["src/path/to/driver.c<br/>Existing Code (~N lines)"]
    
    A["§1.1 First Section"]
    B["§1.2 Second Section"]
    C["§2.1 Third Section"]
    
    SPEC --> A
    DRV --> A
    A --> B
    B --> C
​```
```

#### 5b. Phase-by-Phase Implementation Order table

Group sections into numbered phases. Use this exact table format:

```markdown
### Phase-by-Phase Implementation Order

| Phase  | Sections                         | Depends On                    | Status |
| :----: | -------------------------------- | ----------------------------- | :----: |
| **0**  | Prerequisites (spec, code, etc.) | —                             |   ✅   |
| **1**  | §1.1 Foundation Section          | Phase 0                       |   ⬜   |
| **2**  | §2.1 Core Feature                | Phase 1 (§1.1)                |   ⬜   |
| **3**  | §5.1 Exclusive Feature           | Phase 2 (§2.1)                |   ⬜   |
```

> [!IMPORTANT]
> The Phase table does **not** have a `⭐/💎` icon column. That column is only for
> the OS Comparison table.

#### 5c. Phase notes (NOTE/TIP blocks)

After the table, add detailed notes explaining each phase:

```markdown
> [!NOTE]
> **Phase 0** is already complete — description of current state.
>
> **Phase 1** is the critical path: what it enables and why it must come first.
>
> **Phase 2** delivers key features: list and justify each.
>
> **Phase N** delivers exclusive features (⭐): explain competitive advantage.

> [!TIP]
> **Quick wins after Phase 1:** Describe easy items to implement first.
>
> **Critical gotcha — topic:** Detailed explanation of a non-obvious pitfall.
>
> **QEMU testing flags:** Specific `-device` and `-drive` flags for testing.
>
> **Memory rule reminder:** Reiterate any allocation constraints.
```

### 6. Write the implementation sections

Each numbered section follows this structure:

#### For unimplemented (TODO) sections — use Prompt format:

```markdown
## N. Section Group Title

### N.M Section Title

**Prompt:** Detailed implementation instructions. Explain WHAT to do, WHY, HOW,
and reference the spec section. Include: what to negotiate/detect, what data structure
to build, what API to expose, how to test. End with: "After completing all items,
mark every item as `[x]`, update this prompt to a verification prompt, run
`bash scripts/build.sh clean`, and commit as `"scope: description"`. Add notes
directly in this TODO section. After implementation, save any gotchas, solutions,
and important information to MCP memory."

- [ ] First implementation step
  - [ ] Sub-step with details
  - [ ] Sub-step with register values or constants
- [ ] Second implementation step
- [ ] Wire to higher-level API or filesystem
- [ ] Test: specific QEMU flags or verification command
- [ ] Commit: `"scope: description"`
```

#### For completed sections — use Verification format:

```markdown
### N.M Section Title

**Verification:** Description of what was implemented and how to verify it works.
Include: file names, line numbers, what the serial log should show, build command.

- [x] Completed step (brief description)
- [x] Another completed step

> **Notes:**
> - Implementation detail with file reference and line numbers
> - Gotcha encountered and solution applied
> - QEMU test command that validates this section
```

### 7. Write the Priority Order table

List all sections in priority order:

```markdown
## Priority Order

| Priority  | Section                            | Description                                              |
| --------- | ---------------------------------- | -------------------------------------------------------- |
| 🔴 P0     | N.M Section Name                   | Reason this is critical/blocking                         |
| 🟠 P1     | N.M Section Name                   | Reason this is important                                 |
| 🟡 P2     | N.M Section Name                   | Reason this improves the system                          |
| 🟢 P3     | N.M Section Name                   | Nice-to-have or optimization                             |
| 🟢 P3     | N.M Section Name                   | 🚀 **Exclusive** — brief competitive advantage           |
| 🔵 P4     | N.M Section Name                   | Stretch goal / future compatibility                      |
```

**Priority meanings:**
- 🔴 P0 — Blocking: rules compliance, data integrity, prevents boot
- 🟠 P1 — Important: correctness, production readiness, error handling
- 🟡 P2 — Valuable: performance, SSD optimization, robustness
- 🟢 P3 — Competitive: scalability, telemetry, exclusive features
- 🔵 P4 — Stretch: enterprise, future specs, niche hardware

### 8. Research and add exclusive features

**This step is mandatory.** Before finalizing the TODO, research what Windows 11 and
Linux do for this component, then identify gaps where Impossible OS can do better:

1. **Search the web** for the component's implementation in Windows and Linux
2. **Identify pain points** in both OSes (missing features, poor error handling, no telemetry)
3. **Design Impossible OS exclusives** that address these gaps
4. Mark exclusive features with 🚀 and ⭐ throughout the TODO

**Common exclusive feature categories:**

| Category               | Pattern                                                        |
| ---------------------- | -------------------------------------------------------------- |
| Adaptive behavior      | Dynamic mode switching based on workload metrics               |
| Telemetry              | ns-resolution latency histograms, IOPS counters exposed in GUI |
| Priority mapping       | Win32 I/O priority → hardware queue QoS                        |
| Predictive             | Driver-level prefetch, request merging, pattern detection      |
| Multi-device           | Driver-level striping, mirroring, or aggregation               |
| Hot operations         | Proactive auto-resize, graceful surprise removal               |
| Registry integration   | Tunable parameters via `HKLM\SYSTEM\Drivers\...`              |
| GUI integration        | Real-time dashboards in Disk Manager / Device Manager          |

### 9. Write the OS Comparison table

**This is required for every TODO file.** Use this exact format with emojis:

```markdown
## OS Comparison

| ⭐ | Feature                          | 🪟 Windows 11                     | 🐧 Linux                            | 🚀 Impossible OS                                |
| -- | -------------------------------- | --------------------------------- | ------------------------------------ | ----------------------------------------------- |
| 💎 | Basic feature                    | ✅ How Windows does it             | ✅ How Linux does it                  | ✅ Done — brief description                      |
| 💎 | Partially done feature           | ✅ Windows approach                | ✅ Linux approach                     | ⚠️ §N.M PN — what's missing                     |
| 💎 | Not yet started feature          | ✅ Windows approach                | ✅ Linux approach                     | ⬜ §N.M PN — brief plan                         |
| ⭐ | **Neither has this feature**     | ❌ Not implemented                 | ❌ Not implemented                    | ⬜ §N.M PN — **first to implement** 🚀          |
| 💎 | Only Linux has it                | ⬜ Not supported                   | ✅ Linux approach                     | ⬜ §N.M PN                                      |
| ⭐ | **Exclusive feature name**       | ❌ Short gap description           | ❌ Short gap description              | ⬜ §N.M PN — **competitive advantage** 🚀       |
```

**Status emojis in Impossible OS column:**
- ✅ = Done and working
- ⚠️ = Partially implemented (explain what's missing)
- ⬜ = Not yet started (reference the section and priority)

**After the table, add a summary:**

```markdown
> **After P0+P1 items:** Impossible OS matches Windows and Linux feature-for-feature.
> **After P2–P3 exclusive features:** Exceeds both — list the exclusive advantages.
> **After P4 items:** Full spec parity with enterprise features.
```

### 10. Update the TODO index

Add the new TODO to `todo/TODO-000-INDEX.md` in the correct category section:

```markdown
- [ ] TODO-NNN.NN — Short Title
```

### 11. Commit

```bash
git add -A && git commit -m "todo: add TODO-NNN.NN Short Title roadmap"
```

## Final Checklist

Before committing, verify:
- [ ] File location and numbering are correct
- [ ] Header has a clear goal statement
- [ ] Dependency graph shows all prerequisites and downstream consumers
- [ ] Phase table covers all sections in correct dependency order
- [ ] Phase notes explain each phase's purpose and list gotchas
- [ ] Every section has either a Prompt (unimplemented) or Verification (done) block
- [ ] Every section has checkbox items (`- [ ]` or `- [x]`)
- [ ] Priority Order table lists all sections with correct priorities
- [ ] Exclusive features are marked with 🚀 and ⭐
- [ ] OS Comparison table covers every feature with 🪟/🐧/🚀 columns
- [ ] OS Comparison uses correct emoji status (✅/⬜/⚠️)
- [ ] Table columns are aligned in raw markdown
- [ ] Summary after OS Comparison explains progression
- [ ] TODO index updated
- [ ] No references to `docs/architecture/` (old structure) — use `docs/<domain>/`
- [ ] Spec links point to `specs/` (project root), not `docs/specs/`
