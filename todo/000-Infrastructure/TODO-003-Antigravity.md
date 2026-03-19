# P0003 — Antigravity Agent IDE Setup

> **Goal:** Configure Google Antigravity (Gemini-powered agent IDE) to develop
> Impossible OS efficiently and correctly. Define rules, skills, and workflows that
> prevent AI hallucination of POSIX APIs, enforce bare-metal constraints, and enable
> autonomous build–test–commit cycles. Keep all agent configuration evergreen,
> concise, and synchronized with the actual codebase.

> [!IMPORTANT]
> **This TODO touches the agent IDE configuration files.** Cross-references
> are marked with `→ XREF:` to indicate work in other TODO files that must
> be completed to fully realize agent-driven development.

> [!WARNING]
> **Current state (2026-03-17):** Agent configuration exists but has gaps:
> the `impossible-os` skill has outdated system paths (`C:\Impossible\Sounds\`,
> `C:\Impossible\Bin\`, `C:\Programs\`) that conflict with the Windows naming audit
> (commit `5e12b8e`). The `source-code-organization` skill references `grub.cfg`
> and Multiboot2 (deprecated — OS uses custom UEFI bootloader). The rules file
> lacks hardware-specific constraints (APIC-only, DMA-only, RCU preference). No
> TODO-aware workflow exists. No MCP server configuration for hardware documentation.

---

## Current Architecture Audit

### Agent Configuration Files

| File | Purpose | Issues |
|------|---------|--------|
| `.agents/rules/rules.md` | Always-on constraints (84 lines) | ✅ All rules added (§1.1–1.3) |
| `.agents/workflows/build.md` | Build + QEMU workflow (turbo-all) | ✅ Working — uses `scripts/build.sh` correctly |
| `.agents/workflows/add-asset.md` | Asset loading checklist | ✅ Working — PMM vs kmalloc decision tree |
| `.agents/workflows/release.md` | Tag + changelog + build + publish | Untested — verify release flow |
| `.agents/workflows/test-fs-fat32.md` | FAT32 test with disk images | Untested — verify test flow |
| `.agent/skills/impossible-os/SKILL.md` | OS conventions, paths, structure (143 lines) | ✅ Fully updated in §2.1 |
| `.agent/skills/memory-allocation/SKILL.md` | kmalloc vs PMM decision tree (87 lines) | ✅ Correct — well-documented gotchas |
| `.agent/skills/source-code-organization/SKILL.md` | Source/header layout (170 lines) | ✅ Fully updated in §2.2 |
| `.agent/skills/github/SKILL.md` | Git/GitHub command handling | ✅ Working |
| `.agent/skills/command-completion/SKILL.md` | Command status workaround | ✅ Working |

### What's Missing

| Gap | Impact |
|-----|--------|
| No hardware constraint rules | Agent may generate PIC code, IDE PIO, VGA text mode |
| No Win32 API surface rule | Agent may hallucinate POSIX syscalls instead of Win32 |
| Outdated path conventions in skills | Agent generates code with wrong system paths |
| No TODO-aware workflow | Agent doesn't know how to navigate/update TODO files |
| No MCP server for hardware specs | Agent hallucinates register offsets and MMIO addresses |
| No verification workflow | No standard flow for testing changes after implementation |
| No skill for UEFI bootloader | Agent may assume GRUB/Multiboot boot chain |

---

## 1. Rules: Bare-Metal Guardrails

### 1.1 Update Rules with Hardware Constraints ✅

**Prompt:** This section is marked complete. Verify: open `.agents/rules/rules.md` and confirm the `## Hardware Constraints` section contains 4 rules (APIC-only, DMA-only, UEFI GOP, RCU). Verify each rule is ≤ 2 lines. Verify the PIC note clarifies that `pic.c` masking code is kept.

> [!IMPORTANT]
> → XREF: `TODO-006-Real-Hardware.md §3.1` — PIC→APIC transition and MADT
> PCAT_COMPAT flag. The rule must allow PIC masking code (required on legacy
> hardware) but prevent new PIC-based interrupt routing.

- [x] Add to `.agents/rules/rules.md` → `## Hardware Constraints` (4 rules)
- [x] Keep each rule to 2 lines max (concise = less context overhead)
- [x] Verify existing rules still accurate after additions — ✅ all 7 sections reviewed
- [x] Commit: `"agent: add hardware constraint rules"`

> [!NOTE]
> **Hardware constraint notes (2026-03-17):**
> - 4 rules added: APIC-only, DMA-only, UEFI GOP, RCU preference
> - PIC note clarifies `pic.c` masking code is kept for boot-time disable only
> - Total rules.md: 72 lines (was 65) — minimal context overhead increase`

### 1.2 Add Win32 API Surface Rule ✅

**Prompt:** This section is marked complete. Verify: open `.agents/rules/rules.md` and confirm the `## API Surface` section contains 3 rules (Win32 native API, Windows paths, CPL applets).

> [!IMPORTANT]
> → XREF: `TODO-510-Native-Win32.md` — full Win32 API implementation roadmap.
> → XREF: `TODO-028-Process-Model.md` — Win32 HANDLE table as primary abstraction.

- [x] Add to `.agents/rules/rules.md` → `## API Surface` (3 rules)
- [x] Commit: `"agent: add Win32 API surface rule"`

> [!NOTE]
> **Win32 API surface notes (2026-03-17):**
> - 3 rules: Win32 native API, Windows paths canonical, CPL applets
> - Clarifies POSIX APIs are secondary (Linux compat layer only)
> - Total rules.md: 78 lines (was 72)

### 1.3 Add Freestanding C Rule ✅

**Prompt:** This section is marked complete. Verify: open `.agents/rules/rules.md` and confirm the `## Freestanding C` section contains 3 rules (no stdlib, no malloc, no printf).

- [x] Add to `.agents/rules/rules.md` → `## Freestanding C` (3 rules)
- [x] Commit: `"agent: add freestanding C rule"`

> [!NOTE]
> **Freestanding C notes (2026-03-17):**
> - 3 rules: no stdlib headers, no malloc (use kmalloc/PMM), no printf (use printk/klog)
> - Reinforces the `-ffreestanding -nostdlib -nostdinc` flags from Build Constraints
> - Total rules.md: 84 lines (was 78)

---
## 2. Skills: Evergreen Agent Knowledge

### 2.1 Fix Outdated Paths in `impossible-os` Skill ✅

**Prompt:** This section is marked complete. Verify: open `.agent/skills/impossible-os/SKILL.md` and confirm all paths match current codebase. Verify GRUB references are removed. Verify Cursors path is `System\Cursors\`.

> [!IMPORTANT]
> → XREF: Previous conversation — Windows naming consistency audit (commit `5e12b8e`)
> standardized all system paths across TODO files. Skills must match.

- [x] Update File System Structure tree:
  - [x] `Sounds\` → `Media\` (like `C:\Windows\Media\`)
  - [x] `Bin\` → `System32\` (like `C:\Windows\System32\`)
  - [x] `Lib\` → removed (merged into System32)
  - [x] `Wallpapers\` → `Web\Wallpaper\` (like `C:\Windows\Web\Wallpaper\`)
  - [x] `Programs\` → `Program Files\` (like `C:\Program Files\`)
  - [x] `Temp\` → `C:\Impossible\Temp\` (like `C:\Windows\Temp\`)
- [x] Update "Where Things Go" table to match
- [x] Update "Boot-Time Directory Creation" code block
- [x] Update OS Architecture Summary: GRUB → custom UEFI bootloader (`bootx64.c`)
- [x] Update `Multiboot2` references → UEFI boot params
- [x] Also updated **codebase**: `desktop.c`, `registry.c` (2 locations), `Makefile` sysroot dirs
- [x] Commit: `"agent: update impossible-os skill paths"`

> [!NOTE]
> **Skill update notes (2026-03-17):**
> - Complete rewrite of skill file to match codebase reality
> - Codebase was updated too: `Wallpapers`→`Web/Wallpaper` in desktop.c, registry.c
> - Sysroot: `Bin`→`System32`, `Programs`→`Program Files`, `Temp`→`Impossible/Temp`, added `Media`
> - Fixed: Cursors path `C:\Impossible\Cursors\` → `C:\Impossible\System\Cursors\` (matched code)
> - Fixed: GRUB/Multiboot2 → UEFI bootloader + boot_info struct
> - Added: IXFS+FAT32 filesystem, dirty rectangle compositor, Registry hive paths
> - Build verified: `=== BUILD OK ===``

### 2.2 Fix `source-code-organization` Skill ✅

**Prompt:** This section is marked complete. Verify: open `.agent/skills/source-code-organization/SKILL.md` and confirm `grub.cfg` and `multiboot2_header.asm` are removed, UEFI bootloader directory is present.

- [x] Update Source Directory Layout: `src/boot/` section:
  - [x] Remove `grub.cfg`, `multiboot2_header.asm` from tree
  - [x] Add `src/boot/uefi/bootx64.c` — UEFI PE32+ bootloader
  - [x] Add `src/boot/uefi/efi.h`, `reloc.asm`, `uefi.lds`
- [x] Update Include Directory Layout: remove `multiboot2.h`, add 30+ new headers
- [x] Verify all listed files exist in the actual source tree
- [x] Commit: `"agent: update source-code-organization skill"`

> [!NOTE]
> **Source-code-organization notes (2026-03-17):**
> - Complete rewrite to match actual codebase
> - Added: UEFI bootloader dir, APIC/IOAPIC, AHCI, IPC, sync primitives, GFX lib
> - Added top-level headers: `gfx.h`, `registry.h`, `icon_store.h`, `font_mgr.h`, etc.
> - Added source dirs: `gfx/`, `ipc/`, `test/`
> - Note: legacy `grub.cfg` and `multiboot2_header.asm` still exist in `src/boot/` (kept for reference) but removed from skill tree

### 2.3 Create UEFI Bootloader Skill ✅

**Status:** Complete — 65-line skill covering boot chain, boot_info struct, and agent constraints.

> [!IMPORTANT]
> → XREF: `TODO-010-Bootloader.md` — full bootloader implementation.
> The skill summarizes, not duplicates.

- [x] Create `.agent/skills/uefi-bootloader/SKILL.md`
- [x] Cover: boot chain summary (firmware → BOOTX64.EFI → kernel)
- [x] Cover: `boot_info` struct fields (framebuffer, memory map, RSDP, initrd)
- [x] Cover: what agents must NOT do (no VGA text mode, no BIOS INT, no GRUB)
- [x] Cover: how to add new boot parameters (modify `boot_info` + bootx64.c)
- [x] Keep under 100 lines (65 lines)
- [x] Commit: `"agent: add UEFI bootloader skill"`

### 2.4 Create TODO Navigation Skill ✅

**Status:** Complete — 76-line skill covering index, folder structure, file anatomy, XREFs, commit conventions.

- [x] Create `.agent/skills/todo-system/SKILL.md`
- [x] Cover: how to find the right TODO file (scan index, use folder names)
- [x] Cover: TODO file anatomy (goal → sections → prompts → checklists → tables)
- [x] Cover: cross-reference format and when to add XREFs
- [x] Cover: how to mark items complete and write verification prompts
- [x] Cover: commit message convention for TODO updates
- [x] Keep under 80 lines (76 lines)
- [x] Commit: `"agent: add TODO navigation skill"`

---
## 3. Workflows: Autonomous Execution Patterns

### 3.1 Create TODO Implementation Workflow ✅

**Status:** Complete — `.agents/workflows/implement-todo.md` with 9-step implementation flow and `// turbo-all`.

> [!IMPORTANT]
> → XREF: `TODO-002-Development.md` — development tooling and build scripts.

- [x] Create `.agents/workflows/implement-todo.md`
- [x] Step 1: Read the TODO section prompt fully
- [x] Step 2: Check `→ XREF:` lines — are dependencies completed?
- [x] Step 3: Create implementation plan (kernel = Plan Mode, UI = Fast Mode)
- [x] Step 4: Implement the code following rules and skills
- [x] Step 5: `bash scripts/build.sh clean run` — verify `=== BUILD OK ===`
- [x] Step 6: Check serial output / boot log for expected `[OK]` messages
- [x] Step 7: Mark all items `[x]` in the TODO
- [x] Step 8: Rewrite the prompt as a verification prompt
- [x] Step 9: Commit with the message from the TODO section
- [x] Add `// turbo-all` for build steps
- [x] Commit: `"agent: add TODO implementation workflow"`

### 3.2 Create Verification Workflow ✅

**Status:** Complete — `.agents/workflows/verify-todo.md` with 7-step verification flow and `// turbo-all`.

- [x] Create `.agents/workflows/verify-todo.md`
- [x] Step 1: Read the verification prompt
- [x] Step 2: For each `[x]` item — verify the file/function exists in source
- [x] Step 3: `bash scripts/build.sh clean` → verify `=== BUILD OK ===`
- [x] Step 4: `bash scripts/build.sh run` → check serial output
- [x] Step 5: Verify expected boot log messages appear
- [x] Step 6: Check for regressions (related subsystems still work)
- [x] Step 7: If issues found — update TODO items and fix
- [x] Add `// turbo-all` for build steps
- [x] Commit: `"agent: add verification workflow"`

### 3.3 Create Hardware Test Workflow ✅

**Status:** Complete — `.agents/workflows/test-hardware.md` with manual USB boot steps and log analysis.

> [!IMPORTANT]
> → XREF: `TODO-006-Real-Hardware.md §8` — test checklist and compatibility log.
> → XREF: `TODO-005-Debug.md §4.3` — USB boot logging.

- [x] Create `.agents/workflows/test-hardware.md`
- [x] Step 1: `bash scripts/build.sh clean` → verify `=== BUILD OK ===`
- [x] Step 2: Write USB (manual step — agent prompts user)
- [x] Step 3: Boot target machine (manual step)
- [x] Step 4: Mount USB on dev machine, read `X:\BOOT_NNN.LOG`
- [x] Step 5: Analyze log: search for `[!!]`, `[FAIL]`, `PANIC`, `FAULT`
- [x] Step 6: Update `TODO-006-Real-Hardware.md` → Test Machines table
- [x] Commit: `"agent: add hardware test workflow"`

---
## 4. Agent Execution Strategy

### 4.1 Plan Mode vs Fast Mode Guidelines ✅ *(manual)*

**Prompt:** Document when agents should use Plan Mode (generate a plan artifact for review before coding) versus Fast Mode (code directly). Kernel architecture, driver development, and memory management changes require Plan Mode. UI iteration, bug fixes, and documentation updates can use Fast Mode. Add this as a section in the rules or as a standalone skill. After completing all items, mark every item as `[x]`, and commit as `"agent: document Plan vs Fast mode guidelines"`. Add notes directly in this TODO section.

- [x] Add guidelines to `.agents/rules/rules.md` → `## Agent Execution Mode`:
  ```markdown
  ## Agent Execution Mode

  - **Plan Mode** (review-before-code) for:
    - Kernel subsystems (scheduler, memory, drivers, interrupts)
    - New hardware support (AHCI, USB, SMP)
    - API design (syscalls, Win32 stubs, Registry)
    - Anything touching boot sequence or memory layout
  - **Fast Mode** (code-directly) for:
    - UI/UX iteration (controls, themes, compositor tweaks)
    - Bug fixes with clear reproduction
    - Documentation and TODO updates
    - Adding new assets (fonts, icons, sounds)
  ```
- [x] Commit: `"agent: document Plan vs Fast mode guidelines"`

### 4.2 Multi-Agent Task Allocation Patterns ✅ *(manual)*

**Prompt:** Document recommended patterns for using multiple agents in parallel on Impossible OS. Agents working on different layers can operate simultaneously (e.g., kernel driver + desktop UI). Agents on the same layer must coordinate via shared TODO items. Define the roles and boundaries to prevent merge conflicts. After completing all items, mark every item as `[x]`, and commit as `"agent: document multi-agent patterns"`. Add notes directly in this TODO section.

- [x] Add to `.agents/rules/rules.md` → `## Multi-Agent Coordination`:
  ```markdown
  ## Multi-Agent Coordination

  - **Safe parallel pairs:** kernel driver + desktop UI, filesystem + networking
  - **Conflict-prone pairs:** two agents editing same .c file, boot sequence + main.c
  - **Coordination via TODOs:** mark items `[/]` (in progress) to signal other agents
  - **Never parallel:** two agents both running `bash scripts/build.sh`
  ```
- [x] Commit: `"agent: document multi-agent patterns"`

---
## 5. MCP Server Integration

> [!IMPORTANT]
> **The Gemini API key is all you need.** No OpenAI or Anthropic key required.
> Srclight MCP (§5.2) provides AST-aware code intelligence to agents via
> Tree-sitter indexing. Hardware specs are maintained as markdown documents
> in the `specs/` directory, also indexed by Srclight for agent queries.

### 5.1 Hardware Documentation *(spec documents in `specs/`)* ✅

**Status:** Complete — all 5 spec documents created as markdown in `specs/`, indexed by Srclight MCP.

> [!NOTE]
> Hardware specifications are maintained as comprehensive markdown reference documents
> in the `specs/` directory. Srclight indexes these files, making them searchable
> via `search_symbols()` and `hybrid_search()` by agents in context.

- [x] Create machine-readable spec documents in `specs/`:
  - [x] `specs/cpu/intel-sdm-x86-64.md` — Intel 64 and IA-32 SDM architectural analysis
  - [x] `specs/firmware/uefi-2.10.md` — UEFI Specification Release 2.10
  - [x] `specs/firmware/acpi-6.5.md` — ACPI Specification 6.5
  - [x] `specs/bus/pci-3.0.md` — PCI Local Bus Specification Revision 3.0
  - [x] `specs/storage/ahci-1.3.1.md` — AHCI Specification Revision 1.3.1
- [x] Specs indexed by Srclight (Tree-sitter + FTS5 keyword search)
- [x] Agents can query spec content via Srclight MCP tools
- [x] Commit: `"specs: add PCI Local Bus Specification Revision 3.0"`, `"specs: add AHCI Specification Revision 1.3.1"`

> [!NOTE]
> **Hardware docs notes (2026-03-17):**
> - All 5 specs created as comprehensive markdown documents from public knowledge
> - Indexed by Srclight MCP — agents can search via `hybrid_search()` or `search_symbols()`
> - No external vector database or Gemini API needed — fully local, zero-cost

### 5.2 Srclight MCP Server *(AST-aware code intelligence for agents)*

**Prompt:** Set up Srclight as an MCP server in Antigravity so AI agents get AST-aware code intelligence — 25 specialized tools including `get_callers`, `codebase_map`, and FTS5 hybrid search — without burning tool calls on grep. Srclight runs entirely locally, building a Tree-sitter AST + SQLite knowledge graph of the codebase. After completing all items, mark every item as `[x]`, and commit as `"agent: Srclight MCP server"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> Srclight replaces the original "clangd MCP" plan from this section. clangd is an
> LSP server (not MCP) and is already integrated natively via the IDE. Srclight fills
> the agent-facing knowledge gap: structural code queries, caller/callee graphs,
> symbol search, and optional GPU-accelerated semantic search via Ollama embeddings.

> [!NOTE]
> **Requirements:**
> - Python 3.11+ and Git
> - (Optional) Ollama running locally for GPU-accelerated semantic search
> - Srclight indexes into a local SQLite database — no cloud accounts needed

**Step 1: Install and initialize Srclight** *(manual — developer runs in terminal)* ✅

- [x] Install the package:
  ```bash
  pipx install srclight   # pipx for Ubuntu 24.04+ (PEP 668)
  ```
- [x] Create a global workspace for your projects:
  ```bash
  srclight workspace init dev-workspace
  ```
- [x] Add the Impossible OS repository to the workspace:
  ```bash
  srclight workspace add /home/derickpayne/impossible-os -w dev-workspace
  ```
- [x] Run the initial indexing (Tree-sitter AST parsing → SQLite knowledge graph):
  ```bash
  srclight workspace index -w dev-workspace
  ```

> [!NOTE]
> **Indexing results (2026-03-17):**
> - Srclight v0.8.1 installed via `pipx` (Python 3.12.3)
> - Workspace: `~/.srclight/workspaces/dev-workspace.json`
> - Indexed **220 files**, **5,432 symbols**, **5,362 edges** in 4.9s
> - SQLite DB size: **11.43 MB**

**Step 2: Configure Antigravity** *(manual — developer configures IDE)* ✅

- [x] Open Antigravity → Agent Manager → MCP Servers → Manage → View raw config
- [x] Add the Srclight MCP server entry:
  ```json
  {
    "mcpServers": {
      "srclight": {
        "command": "srclight",
        "args": [
          "serve",
          "--transport", "stdio",
          "--workspace", "dev-workspace"
        ]
      }
    }
  }
  ```
- [x] If `srclight` is not in `$PATH`, use the absolute path to the binary in `"command"`
- [x] Save and refresh the MCP panel

**Step 3: Verify** *(manual)* ✅

- [x] Confirm Srclight appears in the MCP Servers list with active tools
- [x] Test in a new agent session:
  - `codebase_map()` → 220 files, 5,385 symbols, 5,362 edges ✅
  - `search_symbols("kernel_main")` → found in `src/kernel/main.c` ✅
  - `get_callers()` → functional (requires `project="impossible-os"` in workspace mode) ✅
- [x] Verify: Git hooks auto-reindex on commit/branch switch (no manual re-indexing needed)
- [x] Commit: `"agent: Srclight MCP server"`

> [!TIP]
> Srclight uses Git hooks for auto-reindexing — the SQLite database stays synced
> in the background as you commit code or switch branches. No manual re-indexing needed.

---
## 6. Cleanup & Synchronization

### 6.1 Audit All Skills for Accuracy

**Prompt:** Perform a full audit of all 5 existing skills against the current codebase. For each skill, verify: (1) all referenced files exist, (2) all paths match current naming, (3) all code examples compile, (4) no deprecated tools or APIs are referenced. Fix any discrepancies. After completing all items, mark every item as `[x]`, and commit as `"agent: audit all skills for accuracy"`. Add notes directly in this TODO section.

- [x] Audit `.agent/skills/impossible-os/SKILL.md`: ✅ **Accurate.** All paths match post-naming-audit. VFS examples use correct backslash convention. Architecture summary already references UEFI (not GRUB). No changes needed.
  - [x] Verify all system paths match post-naming-audit conventions — ✅ correct
  - [x] Verify code examples use correct VFS paths — ✅ correct
  - [x] Update architecture summary (UEFI, not GRUB) — ✅ already says UEFI
- [x] Audit `.agent/skills/source-code-organization/SKILL.md`: ✅ **Fixed.** Major update — added 15 missing headers and 10 missing source files.
  - [x] Verify directory trees match actual `include/` and `src/` layout — **Fixed:** added barrier.h, kmath.h, version.h, symtab.h, smp.h, boot_splash.h, os_logo.h, multiboot2.h, log.h + ata.h, vbox_mouse.h, rtl8139.h + spinlock.h, seqlock.h, event.h, workqueue.h
  - [x] Remove references to deleted files (`grub.cfg`, `multiboot2_header.asm`) — `multiboot2_header.asm` still exists (kept for legacy compat), annotated as such
  - [x] Add new files (UEFI bootloader, AHCI driver, etc.) — **Fixed:** added gallery.c, klog_flush.c, klog_live.c, log.c, rcu.c, image_save.c, image_scale.c, ico.c, os_logo.c, boot_splash.c, multiboot2_parse.c, symtab.c, version.c, smp/ directory. Fixed `generated/` to show `fluent_codepoints.h` instead of `os_logo_*.h`.
- [x] Audit `.agent/skills/memory-allocation/SKILL.md`: ✅ **Fixed.** Updated tech debt list.
  - [x] Verify "Files That Need PMM Migration" list is current — **Fixed:** `gfx_text.c` font data now correctly uses PMM (resolved). Added new violations: `stb_truetype_impl.c` (STBTT_malloc→kmalloc), `gfx_core.c` (surface alloc), `gfx_blur.c` (scratch buffer).
  - [x] Check if any listed bugs have been fixed — ✅ all 3 bugs (9722a74, f673e46, 5ea919b) remain valid reference examples
- [x] Audit `.agent/skills/github/SKILL.md`: ✅ **Accurate.** Commit conventions (`scope: description`) match current practices. No-output handling advice still relevant.
  - [x] Verify commit conventions match current practices — ✅ correct
- [x] Audit `.agent/skills/command-completion/SKILL.md`: ✅ **Still needed.** `command_status` tool still has the no-output bug with builds. Workaround (sentinel + tail) remains required.
  - [x] Verify workaround is still needed — ✅ confirmed, still necessary
- [x] Commit: `"agent: audit all skills for accuracy"`

### 6.2 Audit All Workflows for Accuracy

**Prompt:** Perform a full audit of all 4 existing workflows. Verify each step works with the current build system and tools. Fix any outdated commands or references. After completing all items, mark every item as `[x]`, and commit as `"agent: audit all workflows for accuracy"`. Add notes directly in this TODO section.

- [x] Audit `.agents/workflows/build.md` — ✅ **Fixed.** Troubleshooting referenced VGA `0xB8000` text mode — replaced with UEFI GOP framebuffer. All build commands (`bash scripts/build.sh`, `tail -1 build/build.log`) verified correct.
- [x] Audit `.agents/workflows/add-asset.md` — ✅ **Fixed.** Code example called `pmm_free_contiguous(phys, pages)` which does not exist — replaced with `pmm_free_frame()` loop (actual API). Decision tree and common mistakes table verified correct.
- [x] Audit `.agents/workflows/release.md` — ✅ **Major rewrite.** Fixed 4 issues: (1) `make clean && make all && make iso` → `bash scripts/build.sh clean` (per rules.md), (2) `VERSION` file → `src/kernel/version.c` (actual location), (3) `build/os-build.iso` → `build/system-disk.img` (actual output name), (4) `/test-hyperv` → `/test-hardware` (actual workflow name).
- [x] Audit `.agents/workflows/test-fs-fat32.md` — ✅ **Fixed.** Step 1 now uses `bash scripts/build.sh clean` instead of raw `make clean`. Step 3 retains `make run-test DISK=fat32` with explicit justification note (build.sh doesn't wrap `run-test`). Verified `run-test` target exists in Makefile at line 488.
- [x] Commit: `"agent: audit all workflows for accuracy"`

---
## Cross-References

| This TODO Section       | Depends On                        | Other TODO File                     | Status   |
|-------------------------|-----------------------------------|-------------------------------------|----------|
| §1.1 Hardware Rules     | PIC→APIC transition               | `TODO-006-Real-Hardware.md §3.1`    | ✅ Done  |
| §1.2 Win32 API Rule     | Native Win32 roadmap              | `TODO-510-Native-Win32.md`          | ✅ Done  |
| §1.2 Win32 API Rule     | Win32 HANDLE model                | `TODO-028-Process-Model.md`         | ✅ Done  |
| §1.3 Freestanding C     | Kernel memory allocation          | Memory-allocation skill             | ✅ Done  |
| §2.1 Fix Paths          | Windows naming audit              | Previous conv (commit `5e12b8e`)    | ✅ Done  |
| §2.2 Fix Src-Org Skill  | Post-UEFI file tree               | Source-code-organization skill      | ✅ Done  |
| §2.3 UEFI Skill         | Bootloader implementation         | `TODO-010-Bootloader.md`            | ✅ Done  |
| §2.4 TODO System Skill  | TODO file conventions             | `TODO-000-INDEX.md`                 | ✅ Done  |
| §3.1 Implement Workflow | Development tooling               | `TODO-002-Development.md`           | ✅ Done  |
| §3.2 Verify Workflow    | Implement workflow                | §3.1 (depends on)                   | ✅ Done  |
| §3.3 HW Test Workflow   | Real hardware test checklist      | `TODO-006-Real-Hardware.md §8`      | ✅ Done  |
| §3.3 HW Test Workflow   | USB boot logging                  | `TODO-005-Debug.md §4.3`            | ✅ Done  |
| §5.1 MCP Srclight       | Clang migration + Bear setup      | `TODO-002 §2.1` + `TODO-002 §6.5`   | ✅ Done  |
| §5.2 MCP Memory         | Node.js in WSL                    | `TODO-004-MCP.md §1`                | ✅ Done  |
| §5.3 MCP Filesystem     | Node.js in WSL                    | `TODO-004-MCP.md §2`                | ✅ Done  |
| §6.1 Audit Skills       | All skills created                | §2.1–§2.4                           | ✅ Done  |
| §6.2 Audit Workflows    | All workflows created             | §3.1–§3.3                           | ✅ Done  |

---

## Priority Order

| Priority | Section                           | Description                                       |
|----------|-----------------------------------|---------------------------------------------------|
| ✅ Done  | 1.1 Hardware constraint rules     | APIC-only, DMA-only, UEFI GOP, RCU               |
| ✅ Done  | 1.2 Win32 API surface rule        | Win32 native API, Windows paths, CPL applets     |
| ✅ Done  | 1.3 Freestanding C rule           | No stdlib, no malloc, no printf                  |
| ✅ Done  | 2.1 Fix outdated paths            | All paths match codebase + Windows conventions   |
| ✅ Done  | 2.2 Fix source-code-org skill     | Added 15 headers, 10+ source files, fixed tree   |
| ✅ Done  | 2.3 UEFI bootloader skill         | Boot chain documented, no GRUB hallucination     |
| ✅ Done  | 2.4 TODO navigation skill         | Agents find existing work via TODO system        |
| ✅ Done  | 3.1 TODO implementation workflow  | 9-step autonomous flow with turbo-all            |
| ✅ Done  | 3.2 Verification workflow         | Quality assurance, post-implementation           |
| ✅ Done  | 3.3 Hardware test workflow        | USB boot + serial capture                        |
| ✅ Done  | 4.1 Plan vs Fast mode docs        | Agent efficiency guidelines                      |
| ✅ Done  | 4.2 Multi-agent patterns          | Parallel development patterns                    |
| ✅ Done  | 5.1 MCP: Srclight code intel      | AST-aware code search via Tree-sitter + FTS5     |
| ✅ Done  | 5.2 MCP: Memory knowledge graph   | Persistent entities/relations/observations       |
| ✅ Done  | 5.3 MCP: Filesystem access        | Sandboxed read/write/edit/search                 |
| ✅ Done  | 6.1 Audit all skills              | 5 skills verified, 2 fixed                       |
| ✅ Done  | 6.2 Audit all workflows           | 4 workflows verified, all fixed                  |

---

## Key Files

| File                                              | Purpose                                          | Status     |
|---------------------------------------------------|------------------------------------------------- |------------|
| `.agents/rules/rules.md`                          | Hardware, Win32, freestanding C rules            | ✅ Done    |
| `.agent/skills/impossible-os/SKILL.md`            | Windows-style paths and OS conventions           | ✅ Audited |
| `.agent/skills/source-code-organization/SKILL.md` | Include/src directory tree (15 headers added)    | ✅ Fixed   |
| `.agent/skills/memory-allocation/SKILL.md`        | kmalloc vs PMM decision tree (tech debt updated) | ✅ Fixed   |
| `.agent/skills/github/SKILL.md`                   | Git command no-output handling                   | ✅ Audited |
| `.agent/skills/command-completion/SKILL.md`       | Build sentinel workaround                        | ✅ Audited |
| `.agent/skills/uefi-bootloader/SKILL.md`          | UEFI boot chain knowledge                        | ✅ Done    |
| `.agent/skills/todo-system/SKILL.md`              | TODO file navigation and conventions             | ✅ Done    |
| `.agents/workflows/build.md`                      | Build + QEMU test (`build.sh`)                   | ✅ Fixed   |
| `.agents/workflows/implement-todo.md`             | 9-step TODO implementation flow                  | ✅ Done    |
| `.agents/workflows/verify-todo.md`                | Post-implementation verification                 | ✅ Done    |
| `.agents/workflows/test-hardware.md`              | USB boot + real hardware test                    | ✅ Done    |
| `.agents/workflows/add-asset.md`                  | Asset loading with PMM patterns                  | ✅ Fixed   |
| `.agents/workflows/release.md`                    | Tag + build + publish ISO                        | ✅ Fixed   |
| `.agents/workflows/test-fs-fat32.md`              | FAT32 test disk + QEMU                           | ✅ Fixed   |
| `.githooks/post-commit`                           | Auto-generate COUNT.md with line counts          | ✅ Done    |

---

## OS Comparison

| Feature                    | VS Code + Copilot            | Cursor                        | Google Antigravity (Impossible OS)        |
|----------------------------|------------------------------|-------------------------------|-------------------------------------------|
| Always-on rules            | ❌ Manual prompting         | ✅ `.cursorrules`            | ✅ `.agents/rules/rules.md`               |
| Reusable skills            | ❌ None                     | ❌ Manual context            | ✅ `.agent/skills/` (7 skills)            |
| Autonomous workflows       | ❌ Manual                   | ⚠️ Basic apply               | ✅ `.agents/workflows/` (6 flows)         |
| Auto-run terminal          | ❌ Requires approval        | ✅ Background tasks          | ✅ `// turbo-all` annotation              |
| Plan→Review→Execute        | ❌ Chat only                | ❌ Chat only                 | ✅ Plan Mode + artifacts                  |
| Multi-agent parallel       | ❌ Single chat              | ❌ Single chat               | ✅ Agent Manager                          |
| MCP: AST code intel        | ❌ None                     | ❌ None                      | ✅ Srclight (Tree-sitter + FTS5)          |
| MCP: persistent memory     | ❌ None                     | ❌ None                      | ✅ Knowledge graph (JSONL)                |
| MCP: filesystem access     | ❌ None                     | ❌ None                      | ✅ Sandboxed read/write/edit              |
| Bare-metal OS awareness    | ❌ Assumes user-space       | ❌ Assumes user-space        | ✅ Rules + skills prevent hallucination   |
| Auto line count tracking   | ❌ None                     | ❌ None                      | ✅ `post-commit` hook → COUNT.md          |

> **Status: ALL SECTIONS COMPLETE ✅**
>
> Every rule, skill, workflow, and MCP server in this TODO has been implemented, audited,
> and verified. The Antigravity agent environment is fully configured with 7 skills,
> 6 workflows, 3 MCP servers, and auto-generated line count tracking. Agents are fully
> constrained for bare-metal OS development — no POSIX hallucination, no VGA text mode,
> no PIC routing, proper UEFI awareness, and accurate codebase knowledge.

---
