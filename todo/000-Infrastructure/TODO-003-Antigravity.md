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
| `.agents/rules/rules.md` | Always-on constraints (65 lines) | Missing: APIC-only rule, DMA-only rule, RCU preference, Win32 API surface |
| `.agents/workflows/build.md` | Build + QEMU workflow (turbo-all) | ✅ Working — uses `scripts/build.sh` correctly |
| `.agents/workflows/add-asset.md` | Asset loading checklist | ✅ Working — PMM vs kmalloc decision tree |
| `.agents/workflows/release.md` | Tag + changelog + build + publish | Untested — verify release flow |
| `.agents/workflows/test-fs-fat32.md` | FAT32 test with disk images | Untested — verify test flow |
| `.agent/skills/impossible-os/SKILL.md` | OS conventions, paths, structure (143 lines) | ❌ **Outdated paths** — pre-naming-audit |
| `.agent/skills/memory-allocation/SKILL.md` | kmalloc vs PMM decision tree (87 lines) | ✅ Correct — well-documented gotchas |
| `.agent/skills/source-code-organization/SKILL.md` | Source/header layout (131 lines) | ❌ **References grub.cfg, Multiboot2** (deprecated) |
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

### 1.1 Update Rules with Hardware Constraints

**Prompt:** The current `rules.md` covers build constraints, code style, and memory allocation gotchas, but lacks hardware-specific rules that prevent the agent from generating incorrect low-level code. Add rules for: (1) APIC-only interrupt routing (no legacy 8259 PIC code), (2) DMA-only storage (no IDE PIO polling), (3) UEFI GOP framebuffer (no VGA text mode), (4) RCU preference for read-heavy data structures. These rules must be concise (one-liners) so they don't bloat the agent context. After completing all items, mark every item as `[x]`, and commit as `"agent: add hardware constraint rules"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-006-Real-Hardware.md §3.1` — PIC→APIC transition and MADT
> PCAT_COMPAT flag. The rule must allow PIC masking code (required on legacy
> hardware) but prevent new PIC-based interrupt routing.

- [ ] Add to `.agents/rules/rules.md` → `## Hardware Constraints`:
  ```markdown
  ## Hardware Constraints

  - **APIC-only interrupts.** Route all hardware interrupts via LAPIC/IOAPIC.
    Do NOT write new 8259 PIC routing code. The PIC is masked at boot.
  - **DMA-only storage.** Use AHCI (DMA + NCQ) or VirtIO for disk I/O.
    Do NOT use legacy IDE/ATA PIO polling (port 0x1F0-0x1F7).
  - **UEFI GOP framebuffer.** The framebuffer is a linear 32bpp buffer
    from UEFI GOP. Do NOT write VGA text mode (0xB8000) code.
  - **RCU for read-heavy structures.** Prefer Read-Copy-Update over
    spinlocks for VFS mount list, process tree, and Registry cache.
  ```
- [ ] Keep each rule to 2 lines max (concise = less context overhead)
- [ ] Verify existing rules still accurate after additions
- [ ] Commit: `"agent: add hardware constraint rules"`

### 1.2 Add Win32 API Surface Rule

**Prompt:** Impossible OS is natively Win32 (PE executables, Win32 API surface). The kernel is ELF, but all user-space APIs should be Windows 11-compatible. Agents must not hallucinate POSIX syscalls (`fork`, `exec`, `open`, `read`) as the primary API — these exist only in the Linux compatibility layer. Add a rule clarifying the API hierarchy. After completing all items, mark every item as `[x]`, and commit as `"agent: add Win32 API surface rule"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-510-Native-Win32.md` — full Win32 API implementation roadmap.
> → XREF: `TODO-028-Process-Model.md` — Win32 HANDLE table as primary abstraction.

- [ ] Add to `.agents/rules/rules.md` → `## API Surface`:
  ```markdown
  ## API Surface

  - **Win32 is the native API.** User-space programs are PE32+ executables
    using Win32-style APIs (CreateFile, ReadFile, CreateProcess). POSIX
    APIs (open, read, fork) are secondary — for the Linux compat layer only.
  - **Windows paths are canonical.** Use `C:\Impossible\System32\`, not
    `/usr/bin/`. Use `C:\Program Files\`, not `/usr/local/`.
  - **Control Panel uses .cpl applets.** Settings are exposed via Windows-
    standard Control Panel Library applets (CPlApplet interface, .cpl extension).
  ```
- [ ] Commit: `"agent: add Win32 API surface rule"`

### 1.3 Add Freestanding C Rule

**Prompt:** The current rules mention `-ffreestanding -nostdlib -nostdinc` compiler flags but don't explicitly prohibit standard library includes. Agents trained on user-space C will reflexively include `<stdio.h>` or use `malloc()`. Add an explicit rule. After completing all items, mark every item as `[x]`, and commit as `"agent: add freestanding C rule"`. Add notes directly in this TODO section.

- [ ] Add to `.agents/rules/rules.md` → `## Freestanding C`:
  ```markdown
  ## Freestanding C

  - **No standard library.** Never include `<stdio.h>`, `<stdlib.h>`,
    `<string.h>`, or any user-space headers. Only freestanding headers
    are allowed: `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<stdarg.h>`.
  - **No malloc().** Use `kmalloc()` (≤ 4 KB) or `pmm_alloc_contiguous()`
    (everything else). See memory-allocation skill for decision tree.
  - **No printf().** Use `printk()` for kernel output, `klog()` for logging.
  ```
- [ ] Commit: `"agent: add freestanding C rule"`

---

## 2. Skills: Evergreen Agent Knowledge

### 2.1 Fix Outdated Paths in `impossible-os` Skill

**Prompt:** The `impossible-os` skill (`.agent/skills/impossible-os/SKILL.md`) contains system paths that were corrected in the Windows naming audit (commit `5e12b8e`). Update all paths to match the current standard: `C:\Impossible\Sounds\` → `C:\Impossible\Media\`, `C:\Impossible\Bin\` → `C:\Impossible\System32\`, `C:\Programs\` → `C:\Program Files\`, `C:\Temp\` → `C:\Impossible\Temp\`, `C:\Impossible\Wallpapers\` → `C:\Impossible\Web\Wallpaper\`. Also update the OS Architecture Summary table: kernel is loaded by custom UEFI bootloader (not GRUB/Multiboot2). After completing all items, mark every item as `[x]`, and commit as `"agent: update impossible-os skill paths"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: Previous conversation — Windows naming consistency audit (commit `5e12b8e`)
> standardized all system paths across TODO files. Skills must match.

- [ ] Update File System Structure tree:
  - [ ] `Sounds\` → `Media\` (like `C:\Windows\Media\`)
  - [ ] `Bin\` → `System32\` (like `C:\Windows\System32\`)
  - [ ] `Lib\` → merged into System32 or `C:\Program Files\ImpossibleSDK\lib\`
  - [ ] `Wallpapers\` → `Web\Wallpaper\` (like `C:\Windows\Web\Wallpaper\`)
  - [ ] `Programs\` → `Program Files\` (like `C:\Program Files\`)
  - [ ] `Temp\` → `C:\Impossible\Temp\` (like `C:\Windows\Temp\`)
- [ ] Update "Where Things Go" table to match
- [ ] Update "Boot-Time Directory Creation" code block
- [ ] Update OS Architecture Summary: GRUB → custom UEFI bootloader (`bootx64.c`)
- [ ] Update `Multiboot2` references → UEFI boot params
- [ ] Commit: `"agent: update impossible-os skill paths"`

### 2.2 Fix `source-code-organization` Skill

**Prompt:** The `source-code-organization` skill references `grub.cfg` in the boot directory tree and `multiboot2.h` / `multiboot2_header.asm` in the include/source layouts. The OS now uses a custom UEFI bootloader (`src/boot/uefi/bootx64.c`). Update the directory trees to reflect the current structure. After completing all items, mark every item as `[x]`, and commit as `"agent: update source-code-organization skill"`. Add notes directly in this TODO section.

- [ ] Update Source Directory Layout: `src/boot/` section:
  - [ ] Remove `grub.cfg`, `multiboot2_header.asm`
  - [ ] Add `src/boot/uefi/bootx64.c` — UEFI PE32+ bootloader
  - [ ] Add `src/boot/uefi/boot_info.h` — boot params structure
- [ ] Update Include Directory Layout: remove `multiboot2.h`, add UEFI boot headers
- [ ] Verify all listed files exist in the actual source tree
- [ ] Commit: `"agent: update source-code-organization skill"`

### 2.3 Create UEFI Bootloader Skill *(NEW)*

**Prompt:** Create a new skill documenting the UEFI bootloader architecture so agents understand the boot chain without hallucinating GRUB or Multiboot. Cover: PE32+ EFI application structure, GOP framebuffer initialization, ACPI RSDP discovery, memory map parsing, kernel ELF loading, `ExitBootServices()` transition, and `boot_info` struct layout. Keep it concise — under 100 lines. After completing all items, mark every item as `[x]`, and commit as `"agent: add UEFI bootloader skill"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-010-Bootloader.md` — full bootloader implementation.
> The skill should summarize, not duplicate.

- [ ] Create `.agent/skills/uefi-bootloader/SKILL.md`
- [ ] Cover: boot chain summary (firmware → BOOTX64.EFI → kernel)
- [ ] Cover: `boot_info` struct fields (framebuffer, memory map, RSDP, initrd)
- [ ] Cover: what agents must NOT do (no VGA text mode, no BIOS INT, no GRUB)
- [ ] Cover: how to add new boot parameters (modify `boot_info` + bootx64.c)
- [ ] Keep under 100 lines
- [ ] Commit: `"agent: add UEFI bootloader skill"`

### 2.4 Create TODO Navigation Skill *(NEW)*

**Prompt:** Create a skill that teaches agents how to navigate, read, and update the TODO system. Cover: the index file (`TODO-000-INDEX.md`), folder structure (`000-Infrastructure/`, `010-Kernel-Foundations/`, etc.), TODO file anatomy (prompt → checklist → commit), cross-reference format (`→ XREF:`), and how to mark items complete (`[x]`). This prevents agents from creating duplicate TODOs or missing existing work items. After completing all items, mark every item as `[x]`, and commit as `"agent: add TODO navigation skill"`. Add notes directly in this TODO section.

- [ ] Create `.agent/skills/todo-system/SKILL.md`
- [ ] Cover: how to find the right TODO file (scan index, use folder names)
- [ ] Cover: TODO file anatomy (goal → sections → prompts → checklists → tables)
- [ ] Cover: cross-reference format and when to add XREFs
- [ ] Cover: how to mark items complete and write verification prompts
- [ ] Cover: commit message convention for TODO updates
- [ ] Keep under 80 lines
- [ ] Commit: `"agent: add TODO navigation skill"`

---

## 3. Workflows: Autonomous Execution Patterns

### 3.1 Create TODO Implementation Workflow *(NEW)*

**Prompt:** Create a workflow that agents follow when implementing a TODO section. Steps: (1) read the TODO section and its prompt, (2) check cross-references for dependencies, (3) plan the implementation (Plan Mode for kernel work, Fast Mode for UI), (4) implement the code, (5) build and test (`bash scripts/build.sh clean run`), (6) verify the boot log, (7) mark items `[x]`, (8) update the prompt to a verification prompt, (9) commit with the specified message. This is the most common agent task. After completing all items, mark every item as `[x]`, and commit as `"agent: add TODO implementation workflow"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-002-Development.md` — development tooling and build scripts.

- [ ] Create `.agents/workflows/implement-todo.md`
- [ ] Step 1: Read the TODO section prompt fully
- [ ] Step 2: Check `→ XREF:` lines — are dependencies completed?
- [ ] Step 3: Create implementation plan (kernel = Plan Mode, UI = Fast Mode)
- [ ] Step 4: Implement the code following rules and skills
- [ ] Step 5: `bash scripts/build.sh clean run` — verify `=== BUILD OK ===`
- [ ] Step 6: Check serial output / boot log for expected `[OK]` messages
- [ ] Step 7: Mark all items `[x]` in the TODO
- [ ] Step 8: Rewrite the prompt as a verification prompt
- [ ] Step 9: Commit with the message from the TODO section
- [ ] Add `// turbo-all` for build steps
- [ ] Commit: `"agent: add TODO implementation workflow"`

### 3.2 Create Verification Workflow *(NEW)*

**Prompt:** Create a workflow for verifying previously implemented TODO sections. Agents should: (1) read the verification prompt, (2) check all `[x]` items match actual source code, (3) build clean, (4) run in QEMU, (5) verify serial output, (6) check for regressions in related subsystems. After completing all items, mark every item as `[x]`, and commit as `"agent: add verification workflow"`. Add notes directly in this TODO section.

- [ ] Create `.agents/workflows/verify-todo.md`
- [ ] Step 1: Read the verification prompt
- [ ] Step 2: For each `[x]` item — verify the file/function exists in source
- [ ] Step 3: `bash scripts/build.sh clean` → verify `=== BUILD OK ===`
- [ ] Step 4: `bash scripts/build.sh run` → check serial output
- [ ] Step 5: Verify expected boot log messages appear
- [ ] Step 6: Check for regressions (related subsystems still work)
- [ ] Step 7: If issues found — update TODO items and fix
- [ ] Add `// turbo-all` for build steps
- [ ] Commit: `"agent: add verification workflow"`

### 3.3 Create Hardware Test Workflow *(NEW)*

**Prompt:** Create a workflow for testing on real hardware via USB boot. Steps: (1) clean build, (2) write USB via `write-usb.ps1` or `.sh`, (3) boot target machine, (4) retrieve `BOOT_NNN.LOG` and `HARDWARE.TXT` from USB, (5) analyze logs for errors, (6) update the hardware compatibility log. After completing all items, mark every item as `[x]`, and commit as `"agent: add hardware test workflow"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-006-Real-Hardware.md §8` — test checklist and compatibility log.
> → XREF: `TODO-005-Debug.md §4.3` — USB boot logging.

- [ ] Create `.agents/workflows/test-hardware.md`
- [ ] Step 1: `bash scripts/build.sh clean` → verify `=== BUILD OK ===`
- [ ] Step 2: Write USB (manual step — agent prompts user)
- [ ] Step 3: Boot target machine (manual step)
- [ ] Step 4: Mount USB on dev machine, read `X:\BOOT_NNN.LOG`
- [ ] Step 5: Analyze log: search for `[!!]`, `[FAIL]`, `PANIC`, `FAULT`
- [ ] Step 6: Update `TODO-006-Real-Hardware.md` → Test Machines table
- [ ] Commit: `"agent: add hardware test workflow"`

---

## 4. Agent Execution Strategy

### 4.1 Plan Mode vs Fast Mode Guidelines

**Prompt:** Document when agents should use Plan Mode (generate a plan artifact for review before coding) versus Fast Mode (code directly). Kernel architecture, driver development, and memory management changes require Plan Mode. UI iteration, bug fixes, and documentation updates can use Fast Mode. Add this as a section in the rules or as a standalone skill. After completing all items, mark every item as `[x]`, and commit as `"agent: document Plan vs Fast mode guidelines"`. Add notes directly in this TODO section.

- [ ] Add guidelines to `.agents/rules/rules.md` → `## Agent Execution Mode`:
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
- [ ] Commit: `"agent: document Plan vs Fast mode guidelines"`

### 4.2 Multi-Agent Task Allocation Patterns

**Prompt:** Document recommended patterns for using multiple agents in parallel on Impossible OS. Agents working on different layers can operate simultaneously (e.g., kernel driver + desktop UI). Agents on the same layer must coordinate via shared TODO items. Define the roles and boundaries to prevent merge conflicts. After completing all items, mark every item as `[x]`, and commit as `"agent: document multi-agent patterns"`. Add notes directly in this TODO section.

- [ ] Add to `.agents/rules/rules.md` → `## Multi-Agent Coordination`:
  ```markdown
  ## Multi-Agent Coordination

  - **Safe parallel pairs:** kernel driver + desktop UI, filesystem + networking
  - **Conflict-prone pairs:** two agents editing same .c file, boot sequence + main.c
  - **Coordination via TODOs:** mark items `[/]` (in progress) to signal other agents
  - **Never parallel:** two agents both running `bash scripts/build.sh`
  ```
- [ ] Commit: `"agent: document multi-agent patterns"`

---

## 5. MCP Server Integration

> [!IMPORTANT]
> **The Gemini API key is all you need.** No OpenAI or Anthropic key required.
> The semantic search tool natively supports Gemini's `text-embedding-004` model,
> and Gemini's massive 1M+ token context window excels at digesting intricate
> relationships in the Impossible OS codebase.
>
> This section defines a **3-phase intelligence pipeline:**
> - **Phase 1:** Clang/LLD migration (`TODO-002 §2.1`) — produces Clang-compatible build
> - **Phase 2:** clangd MCP (`TODO-002 §6.5`) — C/C++ code intelligence
> - **Phase 3:** Gemini semantic search (this section) — AI-powered codebase search

### 5.1 Hardware Documentation Server *(Stretch)*

**Prompt:** Configure an MCP server that serves the Intel x86-64 SDM, UEFI Specification, and ACPI specification as queryable resources. This allows agents to look up exact register offsets, bit definitions, and protocol sequences instead of hallucinating them. This is a stretch goal — initial development can rely on inline comments referencing manual sections. After completing all items, mark every item as `[x]`, and commit as `"agent: MCP server for hardware docs"`. Add notes directly in this TODO section.

> [!NOTE]
> MCP is Model Context Protocol — a way to provide external knowledge sources to
> AI agents. This section is aspirational and depends on Antigravity's MCP support
> and available documentation in machine-readable format.

- [ ] *(Stretch)* Acquire machine-readable versions of:
  - [ ] Intel 64 and IA-32 SDM (Volumes 1-4)
  - [ ] UEFI Specification 2.10
  - [ ] ACPI Specification 6.5
  - [ ] PCI Local Bus Specification 3.0
  - [ ] AHCI Specification 1.3.1
- [ ] *(Stretch)* Configure MCP server in Antigravity settings
- [ ] *(Stretch)* Test: agent queries "What is the LAPIC timer LVT register offset?"
- [ ] *(Stretch)* Verify: agent returns correct answer (0x320) without hallucination
- [ ] Commit: `"agent: MCP server for hardware docs"`

### 5.2 clangd MCP Server *(flawless C code manipulation)*

**Prompt:** Wire clangd as an MCP server in Antigravity so the AI agent gets byte-accurate C/C++ symbol resolution, go-to-definition, and diagnostics directly in the agent context. This requires `compile_commands.json` from Phase 2 (`TODO-002 §6.5`). After completing all items, mark every item as `[x]`, and commit as `"agent: clangd MCP server"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-002 §2.1` — Clang/LLD migration must be complete first (Phase 1).
> → XREF: `TODO-002 §6.5` — Bear + `.clangd` config must be set up first (Phase 2).
> The `compile_commands.json` must exist at the repo root before this MCP server works.

**Step 1: Add the clangd MCP server entry** *(manual — developer configures IDE)*

- [ ] Open Antigravity → Agent Manager → MCP Servers → Manage → View raw config
- [ ] Add this server entry to the `mcpServers` object:
  ```json
  "impossible-os-clangd": {
    "command": "clangd",
    "args": [
      "--background-index",
      "--compile-commands-dir=.",
      "--log=error"
    ]
  }
  ```
- [ ] Save and refresh the MCP panel

**Step 2: Verify** *(manual)*

- [ ] Verify the MCP panel shows `impossible-os-clangd` as connected
- [ ] Tell the agent: "Use clangd to find all callers of `printk`" → accurate results
- [ ] Tell the agent: "What fields does `struct boot_info` have?" → correct answer from source
- [ ] Commit: `"agent: clangd MCP server"`

### 5.3 Gemini Semantic Search *(AI-powered codebase context)*

**Prompt:** Set up Gemini-powered semantic search over the entire Impossible OS codebase using the `@zilliz/claude-context-mcp` package. Despite the name, this package natively maps to the Gemini API for vector embeddings via `text-embedding-004`. The codebase is converted into vector embeddings and stored in a Zilliz Cloud serverless cluster, enabling natural-language queries like "how does the bootloader pass the memory map to the kernel" — the agent gets semantically relevant code snippets as context. This is the ultimate dual-indexer: **clangd for C code manipulation, Gemini for architecture-level search**. After completing all items, mark every item as `[x]`, and commit as `"agent: Gemini semantic search MCP"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-002 §2.1` — Clang migration (Phase 1) must be complete.
> → XREF: `TODO-002 §6.5` — clangd + Bear (Phase 2) must be operational.
> → XREF: `TODO-003 §5.2` — clangd MCP (this file) should be configured first.
> This is Phase 3 — the final layer of the intelligence pipeline.

> [!NOTE]
> **Requirements:**
> - A **Gemini API key** (the only API key needed — no OpenAI or Anthropic required)
> - A free **Zilliz Cloud** serverless cluster (vector database for embeddings)
> - The `compile_commands.json` from Phase 2 (for accurate indexing)

**Step 1: Create Zilliz Cloud vector database** *(manual — developer sets up cloud account)*

- [ ] Go to [Zilliz Cloud](https://cloud.zilliz.com) and create a free account
- [ ] Spin up a **free Serverless cluster** (no credit card required for free tier)
- [ ] Note your credentials:
  - `MILVUS_ADDRESS` — the cluster endpoint URL
  - `MILVUS_TOKEN` — the API token for authentication
- [ ] Store these securely (do NOT commit to the repo)

**Step 2: Configure the unified MCP server JSON** *(manual — developer configures IDE)*

- [ ] Open Antigravity → Agent Manager → MCP Servers → Manage → View raw config
- [ ] Paste the **complete unified MCP configuration** (combines clangd from §5.2 + semantic search):
  ```json
  {
    "mcpServers": {
      "impossible-os-clangd": {
        "command": "clangd",
        "args": [
          "--background-index",
          "--compile-commands-dir=.",
          "--log=error"
        ]
      },
      "semantic-context": {
        "command": "npx",
        "args": ["-y", "@zilliz/claude-context-mcp@latest"],
        "env": {
          "GEMINI_API_KEY": "<Your-Gemini-API-Key>",
          "EMBEDDING_MODEL": "text-embedding-004",
          "MILVUS_ADDRESS": "<Your-Zilliz-Cluster-URL>",
          "MILVUS_TOKEN": "<Your-Zilliz-Token>"
        }
      }
    }
  }
  ```
- [ ] Replace `<Your-Gemini-API-Key>` with your actual Gemini API key
- [ ] Replace `<Your-Zilliz-Cluster-URL>` with your Zilliz cluster endpoint
- [ ] Replace `<Your-Zilliz-Token>` with your Zilliz API token
- [ ] Save and refresh the MCP panel

**Step 3: Index the entire workspace** *(manual — developer triggers via agent prompt)*

- [ ] After saving the MCP config, tell the Antigravity agent:
  > _"Use your semantic context tool to index this entire workspace. Ensure you
  > explicitly include all `.c`, `.h`, `.asm`, `.ld`, and `.sh` files."_
- [ ] Wait for the indexing to complete (converts codebase into Gemini vector embeddings)
- [ ] Verify the Zilliz Cloud dashboard shows indexed documents

**Step 4: Verify the dual-indexer pipeline** *(manual)*

- [ ] Test **clangd** (exact code manipulation):
  - Ask: "What are the fields of `struct vfs_node`?" → exact struct definition from C source
  - Ask: "Find all callers of `blkdev_read()`" → precise call sites with line numbers
- [ ] Test **semantic search** (architecture-level understanding):
  - Ask: "How does the bootloader pass the memory map to the kernel?" → relevant code snippets from `bootx64.c`, `kernel_main`, and `boot_info` struct
  - Ask: "What happens during the SMP bring-up sequence?" → finds `smp.asm`, `smp.c`, trampoline code
  - Ask: "How does the compositor avoid flickering during window drags?" → finds dirty rect tracker, `fb_swap_rect()`, preemption guards
- [ ] Verify: no hallucinated function names or incorrect register offsets
- [ ] Commit: `"agent: Gemini semantic search MCP"`

---

## 6. Cleanup & Synchronization

### 6.1 Audit All Skills for Accuracy

**Prompt:** Perform a full audit of all 5 existing skills against the current codebase. For each skill, verify: (1) all referenced files exist, (2) all paths match current naming, (3) all code examples compile, (4) no deprecated tools or APIs are referenced. Fix any discrepancies. After completing all items, mark every item as `[x]`, and commit as `"agent: audit all skills for accuracy"`. Add notes directly in this TODO section.

- [ ] Audit `.agent/skills/impossible-os/SKILL.md`:
  - [ ] Verify all system paths match post-naming-audit conventions
  - [ ] Verify code examples use correct VFS paths
  - [ ] Update architecture summary (UEFI, not GRUB)
- [ ] Audit `.agent/skills/source-code-organization/SKILL.md`:
  - [ ] Verify directory trees match actual `include/` and `src/` layout
  - [ ] Remove references to deleted files (`grub.cfg`, `multiboot2_header.asm`)
  - [ ] Add new files (UEFI bootloader, AHCI driver, etc.)
- [ ] Audit `.agent/skills/memory-allocation/SKILL.md`:
  - [ ] Verify "Files That Need PMM Migration" list is current
  - [ ] Check if any listed bugs have been fixed
- [ ] Audit `.agent/skills/github/SKILL.md`:
  - [ ] Verify commit conventions match current practices
- [ ] Audit `.agent/skills/command-completion/SKILL.md`:
  - [ ] Verify workaround is still needed
- [ ] Commit: `"agent: audit all skills for accuracy"`

### 6.2 Audit All Workflows for Accuracy

**Prompt:** Perform a full audit of all 4 existing workflows. Verify each step works with the current build system and tools. Fix any outdated commands or references. After completing all items, mark every item as `[x]`, and commit as `"agent: audit all workflows for accuracy"`. Add notes directly in this TODO section.

- [ ] Audit `.agents/workflows/build.md` — verify all commands work
- [ ] Audit `.agents/workflows/add-asset.md` — verify PMM patterns current
- [ ] Audit `.agents/workflows/release.md` — verify release flow
- [ ] Audit `.agents/workflows/test-fs-fat32.md` — verify test disk images
- [ ] Commit: `"agent: audit all workflows for accuracy"`

---

## Cross-References

| This TODO Section       | Depends On                        | Other TODO File                    |
|-------------------------|-----------------------------------|------------------------------------|
| §1.1 Hardware Rules     | PIC→APIC transition              | `TODO-006-Real-Hardware.md §3.1`  |
| §1.2 Win32 API Rule     | Native Win32 roadmap             | `TODO-510-Native-Win32.md`         |
| §1.2 Win32 API Rule     | Win32 HANDLE model               | `TODO-028-Process-Model.md`       |
| §2.1 Fix Paths          | Windows naming audit             | Previous conv (commit `5e12b8e`)  |
| §2.3 UEFI Skill         | Bootloader implementation        | `TODO-010-Bootloader.md`          |
| §3.1 Implement Workflow | Development tooling              | `TODO-002-Development.md`         |
| §3.3 HW Test Workflow   | Real hardware test checklist     | `TODO-006-Real-Hardware.md §8`    |
| §3.3 HW Test Workflow   | USB boot logging                 | `TODO-005-Debug.md §4.3`         |
| §5.1 MCP Hardware       | Hardware specs (external)        | N/A — external documentation      |
| §5.2 clangd MCP         | Clang migration + Bear setup     | `TODO-002 §2.1` + `TODO-002 §6.5`|
| §5.3 Gemini Semantic     | All Phase 1-2 intelligence       | `TODO-002 §2.1` + `TODO-002 §6.5`|

---

## Priority Order

| Priority | Section                         | Description                                    |
|----------|---------------------------------|------------------------------------------------|
| 🔴 P0   | 2.1 Fix outdated paths           | Skills generate wrong code — immediate harm    |
| 🔴 P0   | 2.2 Fix source-code-org skill    | References deleted files (grub.cfg)            |
| 🔴 P0   | 6.1 Audit all skills             | Catch any other stale content                  |
| 🟠 P1   | 1.1 Hardware constraint rules    | Prevent PIC/PIO/VGA hallucination              |
| 🟠 P1   | 1.2 Win32 API surface rule       | Prevent POSIX hallucination                    |
| 🟠 P1   | 1.3 Freestanding C rule          | Prevent stdlib includes                        |
| 🟠 P1   | 2.3 UEFI bootloader skill        | Prevent GRUB hallucination                     |
| 🟠 P1   | 3.1 TODO implementation workflow | Most common agent task                         |
| 🟠 P1   | 5.2 clangd MCP server            | Phase 2 intelligence — after TODO-002 §6.5    |
| 🟠 P1   | 5.3 Gemini semantic search       | Phase 3 intelligence — after §5.2             |
| 🟡 P2   | 2.4 TODO navigation skill        | Helps agents find existing work                |
| 🟡 P2   | 3.2 Verification workflow        | Quality assurance                              |
| 🟡 P2   | 4.1 Plan vs Fast mode docs       | Agent efficiency                               |
| 🟡 P2   | 4.2 Multi-agent patterns         | Parallel development                           |
| 🟡 P2   | 6.2 Audit all workflows          | Maintenance                                    |
| 🟢 P3   | 3.3 Hardware test workflow       | Depends on USB scripts existing                |
| 🟢 P3   | 5.1 MCP hardware docs            | Stretch — depends on MCP availability          |

---

## Key Files

| File                                              | Purpose                                    |
|---------------------------------------------------|--------------------------------------------|
| `.agents/rules/rules.md`                         | [MODIFY] Add hardware, Win32, freestanding rules |
| `.agent/skills/impossible-os/SKILL.md`           | [MODIFY] Fix outdated system paths         |
| `.agent/skills/source-code-organization/SKILL.md`| [MODIFY] Fix deprecated boot references    |
| `.agent/skills/uefi-bootloader/SKILL.md`         | [NEW] UEFI boot chain knowledge            |
| `.agent/skills/todo-system/SKILL.md`             | [NEW] TODO navigation skill                |
| `.agents/workflows/implement-todo.md`            | [NEW] TODO implementation workflow          |
| `.agents/workflows/verify-todo.md`               | [NEW] Verification workflow                |
| `.agents/workflows/test-hardware.md`             | [NEW] Hardware test workflow               |

---

## OS Comparison

| Feature                    | VS Code + Copilot          | Cursor                      | Google Antigravity (Impossible OS) |
|----------------------------|----------------------------|-----------------------------|--------------------------------------|
| Always-on rules            | ❌ Manual prompting         | ✅ `.cursorrules`            | ✅ `.agents/rules/rules.md`         |
| Reusable skills            | ❌ None                     | ❌ Manual context            | ✅ `.agent/skills/` (5 skills)      |
| Autonomous workflows       | ❌ Manual                   | ⚠️ Basic apply              | ✅ `.agents/workflows/` (4 flows)   |
| Auto-run terminal          | ❌ Requires approval        | ✅ Background tasks          | ✅ `// turbo-all` annotation        |
| Plan→Review→Execute       | ❌ Chat only                | ❌ Chat only                 | ✅ Plan Mode + artifacts            |
| Multi-agent parallel       | ❌ Single chat              | ❌ Single chat               | ✅ Agent Manager                    |
| MCP: clangd code intel     | ❌ None                     | ❌ None                      | ⬜ §5.2 P1 — clangd MCP server      |
| MCP: semantic search       | ❌ None                     | ❌ None                      | ⬜ §5.3 P1 — Gemini + Zilliz        |
| MCP: hardware docs         | ❌ None                     | ❌ None                      | ⬜ §5.1 P3 — stretch                |
| Bare-metal OS awareness    | ❌ Assumes user-space        | ❌ Assumes user-space         | ✅ Rules + skills prevent hallucination |

> **After P0 items:** All skills and rules accurately reflect the current codebase — agents
> generate correct code without producing stale paths or deprecated patterns.
> **After P1 items:** Agents are fully constrained for bare-metal OS development — no
> POSIX hallucination, no VGA text mode, no PIC routing, proper UEFI awareness.
> **After P2+P3 items:** Fully autonomous agent-driven development with predictable workflows,
> parallel execution, and hardware documentation grounding.

---
