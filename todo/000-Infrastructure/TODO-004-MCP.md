# TODO-004 — MCP Server Setup

> **Goal:** Install and configure two additional MCP servers for Antigravity:
> - **@modelcontextprotocol/server-memory** — persistent knowledge graph memory for agents
> - **@modelcontextprotocol/server-filesystem** — controlled filesystem access for agents
>
> → XREF: `TODO-003-Antigravity.md §5` — MCP server ecosystem (Srclight already configured)

---

## 1. @modelcontextprotocol/server-memory — Knowledge Graph Memory

> [!NOTE]
> **What:** Persistent knowledge graph that stores entities, relations, and observations.
> Agents use this to remember facts across conversations — project decisions, user
> preferences, architectural patterns, debugging context, etc.
> **GitHub:** https://github.com/modelcontextprotocol/servers/tree/main/src/memory
> **Transport:** stdio (npx)
> **Storage:** JSONL file (configurable path)

### 1.1 Prerequisites ✅

- [x] Verify Node.js is installed: `node --version` → `v18.19.1` ✅
- [x] Installed via: `sudo apt-get install -y nodejs`
- [x] Verify npx is available: `npx --version` → `11.9.0` ✅

### 1.2 Test Server Launch ✅

- [x] Verify the server runs:
  ```bash
  npx -y @modelcontextprotocol/server-memory
  ```
- [x] Confirm no npm errors or missing dependencies — launches cleanly as stdio server

### 1.3 Choose Memory File Location ✅

- [x] Decision: **Project-local** — `~/impossible-os/.memory/memory.jsonl` (gitignored)
- [x] Create the directory:
  ```bash
  mkdir -p ~/impossible-os/.memory
  ```
- [x] Add `.memory/` to `.gitignore`

### 1.4 Configure Antigravity MCP

- [ ] Add to Antigravity's MCP config (Agent Manager → MCP Servers → View raw config):
  ```json
  {
    "mcpServers": {
      "memory": {
        "command": "npx",
        "args": [
          "-y",
          "@modelcontextprotocol/server-memory"
        ],
        "env": {
          "MEMORY_FILE_PATH": "/home/derickpayne/impossible-os/.memory/memory.jsonl"
        }
      }
    }
  }
  ```
- [ ] Save and refresh MCP panel
- [ ] Verify: `memory` appears in MCP Servers list with tools:
  - `create_entities` — add new entities to the knowledge graph
  - `create_relations` — define relationships between entities
  - `add_observations` — attach facts to entities
  - `delete_entities` — remove entities
  - `delete_observations` — remove specific observations
  - `delete_relations` — remove relationships
  - `read_graph` — read the entire knowledge graph
  - `search_nodes` — search for entities by name or observation
  - `open_nodes` — retrieve specific entities by name

### 1.5 Test Memory Operations

- [ ] Test entity creation — agent creates a test entity:
  ```
  Create entity: name="Impossible_OS", type="project",
  observations=["Custom x86-64 OS", "UEFI boot", "Windows-style API"]
  ```
- [ ] Test search — agent searches for "Impossible":
  - Should return the entity created above
- [ ] Test relation creation:
  ```
  Create relation: from="Impossible_OS", to="AHCI", type="uses_driver"
  ```
- [ ] Verify: `read_graph` returns all entities and relations
- [ ] Clean up test data or keep as seed knowledge

### 1.6 Seed Initial Knowledge *(optional)*

> [!NOTE]
> Pre-populate the knowledge graph with key project facts so agents
> start with baseline context in new conversations.

- [ ] Seed core project entities:
  - `Impossible_OS` (project) — architecture, boot chain, API style
  - `UEFI_Bootloader` (component) — PE32+, GOP, ExitBootServices
  - `Kernel` (component) — x86-64, Long Mode, identity-mapped
  - `PMM` (allocator) — physical memory, large allocations
  - `kmalloc` (allocator) — ≤4 KB kernel bookkeeping only
  - `Srclight` (tool) — code intelligence MCP server
- [ ] Seed key relations:
  - `Impossible_OS` → `uses` → `UEFI_Bootloader`
  - `Kernel` → `allocates_with` → `PMM`
  - `Kernel` → `allocates_with` → `kmalloc`
- [ ] Seed critical observations (known gotchas from rules.md):
  - `kmalloc`: "Only for small kernel bookkeeping ≤4 KB. NEVER for buffers."
  - `PMM`: "Use pmm_alloc_contiguous() for anything >4 KB."

- [ ] Commit: `"agent: configure server-memory MCP"`

---

## 2. @modelcontextprotocol/server-filesystem — Filesystem Access

> [!NOTE]
> **What:** Controlled filesystem access for agents — read, write, edit, search, and
> list files within explicitly allowed directories. Agents can only access paths
> specified in the configuration.
> **GitHub:** https://github.com/modelcontextprotocol/servers/tree/main/src/filesystem
> **Transport:** stdio (npx)

### 2.1 Prerequisites ✅

- [x] Node.js `v18.19.1` ✅ (installed in §1.1)
- [x] npx `11.9.0` ✅ (verified in §1.1)
- [x] Server launches cleanly: `npx -y @modelcontextprotocol/server-filesystem /path`

### 2.2 Decide Allowed Directories

> [!IMPORTANT]
> The filesystem server ONLY allows access to directories explicitly listed in args.
> Agents cannot escape these sandboxed paths. Choose carefully.

- [ ] Determine which directories agents should access:
  - `/home/derickpayne/impossible-os` — the project root (required)
  - `/home/derickpayne/impossible-os/specs` — hardware spec documents
  - `/home/derickpayne/impossible-os/docs` — project documentation
  - *(add other paths as needed)*

### 2.3 Configure Antigravity MCP

- [ ] Add to Antigravity's MCP config (Agent Manager → MCP Servers → View raw config):
  ```json
  {
    "mcpServers": {
      "filesystem": {
        "command": "npx",
        "args": [
          "-y",
          "@modelcontextprotocol/server-filesystem",
          "/home/derickpayne/impossible-os"
        ]
      }
    }
  }
  ```
- [ ] Save and refresh MCP panel
- [ ] Verify: `filesystem` appears in MCP Servers list with tools:
  - `read_text_file` — read file contents (supports head/tail)
  - `read_media_file` — read images/audio as base64
  - `read_multiple_files` — batch read multiple files
  - `write_file` — create or overwrite files
  - `edit_file` — selective edits with pattern matching and diff preview
  - `create_directory` — create directories (with parents)
  - `list_directory` — list files/dirs with type prefixes
  - `list_directory_with_sizes` — list with file sizes and sorting
  - `move_file` — move or rename files/directories
  - `search_files` — recursive file search by name pattern
  - `get_file_info` — file metadata (size, timestamps, permissions)
  - `list_allowed_directories` — show configured sandboxed paths

### 2.4 Test Filesystem Operations

- [ ] Test read: agent reads `include/kernel/boot_info.h`
  - Should return file contents
- [ ] Test list: agent lists `src/kernel/` directory
  - Should show files with `[FILE]`/`[DIR]` prefixes
- [ ] Test search: agent searches for files matching `*.h` in `include/`
  - Should return header files
- [ ] Test boundary: agent attempts to read `/etc/passwd`
  - Should fail with access denied (outside allowed directories)

- [ ] Commit: `"agent: configure server-filesystem MCP"`

---

## 3. Verification — All MCP Servers Active

- [ ] Verify all 3 MCP servers appear in Antigravity:
  | Server | Transport | Purpose |
  |---|---|---|
  | `srclight` | stdio | AST-aware code intelligence |
  | `memory` | stdio | Persistent knowledge graph |
  | `filesystem` | stdio | Sandboxed file access |
- [ ] Test combined workflow:
  1. Agent searches code via `srclight` → finds AHCI driver
  2. Agent reads file via `filesystem` → gets full source
  3. Agent stores finding in `memory` → creates entity for future reference
- [ ] Commit: `"agent: verify all MCP servers operational"`
