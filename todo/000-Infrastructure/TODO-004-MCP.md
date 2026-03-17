# TODO-004 — MCP Server Setup ✅

> **Goal:** Install and configure two additional MCP servers for Antigravity:
> - **@modelcontextprotocol/server-memory** — persistent knowledge graph memory for agents
> - **@modelcontextprotocol/server-filesystem** — controlled filesystem access for agents
>
> → XREF: `TODO-003-Antigravity.md §5` — MCP server ecosystem (Srclight already configured)

> [!CAUTION]
> **Windows WSL gotcha (2026-03-17):**
> Antigravity runs on Windows but executes MCP commands in WSL. The default `npx` in
> PATH resolves to Windows Node.js v24 (`/mnt/c/Program Files/nodejs/npx`), which has
> a breaking ESM change (`ERR_UNSUPPORTED_DIR_IMPORT` in zod). **Do NOT use `npx` in
> MCP configs.** Instead, install packages locally in WSL and use `/usr/bin/node` with
> absolute paths to the `dist/index.js` entry points.

---

## 1. @modelcontextprotocol/server-memory — Knowledge Graph Memory ✅

> [!NOTE]
> **What:** Persistent knowledge graph that stores entities, relations, and observations.
> Agents use this to remember facts across conversations — project decisions, user
> preferences, architectural patterns, debugging context, etc.
> **GitHub:** https://github.com/modelcontextprotocol/servers/tree/main/src/memory
> **Transport:** stdio
> **Storage:** JSONL file at `~/.memory/memory.jsonl`

### 1.1 Prerequisites ✅

- [x] Verify Node.js is installed: `node --version` → `v18.19.1` ✅
- [x] Installed via: `sudo apt-get install -y nodejs`
- [x] Install npm in WSL: `sudo apt-get install -y npm`
- [x] Verify npx is available: `npx --version` → `9.2.0` (WSL) ✅

### 1.2 Install Packages Locally ✅

> [!IMPORTANT]
> **Do NOT use `npx -y` in MCP config.** The `npx` in `$PATH` resolves to
> Windows Node.js v24 which breaks the MCP SDK's zod ESM imports.
> Instead, install packages to a local directory and use `/usr/bin/node` directly.

- [x] Create package directory:
  ```bash
  mkdir -p ~/mcp-servers && cd ~/mcp-servers
  /usr/bin/npm init -y
  /usr/bin/npm install @modelcontextprotocol/server-memory @modelcontextprotocol/server-filesystem
  ```
- [x] Verify installation:
  ```
  ~/mcp-servers/node_modules/@modelcontextprotocol/server-memory/dist/index.js ✅
  ~/mcp-servers/node_modules/@modelcontextprotocol/server-filesystem/dist/index.js ✅
  ```
- [x] Test launch: `/usr/bin/node .../server-memory/dist/index.js` → "Knowledge Graph MCP Server running on stdio" ✅

### 1.3 Memory File Location ✅

- [x] Decision: **Project-local** — `/home/derickpayne/impossible-os/.memory/memory.jsonl`
- [x] Created directory: `mkdir -p ~/impossible-os/.memory`
- [x] Added `.memory/` to `.gitignore`

### 1.4 Configure Antigravity MCP ✅

- [x] Added to `~/.gemini/antigravity/mcp_config.json`:
  ```json
  "memory": {
      "command": "/usr/bin/node",
      "args": [
          "/home/derickpayne/mcp-servers/node_modules/@modelcontextprotocol/server-memory/dist/index.js"
      ],
      "env": {
          "MEMORY_FILE_PATH": "/home/derickpayne/impossible-os/.memory/memory.jsonl"
      }
  }
  ```
- [x] Server appears in Antigravity MCP panel ✅
- [x] Tools available:
  - `create_entities` — add entities to the knowledge graph
  - `create_relations` — define relationships between entities
  - `add_observations` — attach facts to entities
  - `delete_entities` / `delete_observations` / `delete_relations` — remove data
  - `read_graph` — read the entire knowledge graph
  - `search_nodes` — search by name or observation content
  - `open_nodes` — retrieve specific entities by name

---

## 2. @modelcontextprotocol/server-filesystem — Filesystem Access ✅

> [!NOTE]
> **What:** Controlled filesystem access for agents — read, write, edit, search, and
> list files within explicitly allowed directories. Agents cannot escape the sandboxed paths.
> **GitHub:** https://github.com/modelcontextprotocol/servers/tree/main/src/filesystem
> **Transport:** stdio

### 2.1 Prerequisites ✅

- [x] Node.js `v18.19.1` ✅ (installed in §1.1)
- [x] npm `9.2.0` ✅ (installed in §1.1)
- [x] Package installed in `~/mcp-servers/` (same install as §1.2)

### 2.2 Allowed Directories ✅

- [x] Configured: `/home/derickpayne/impossible-os` — full project root access
  - Includes all source code, specs, docs, TODO files
  - Agents cannot access directories outside this path

### 2.3 Configure Antigravity MCP ✅

- [x] Added to `~/.gemini/antigravity/mcp_config.json`:
  ```json
  "filesystem": {
      "command": "/usr/bin/node",
      "args": [
          "/home/derickpayne/mcp-servers/node_modules/@modelcontextprotocol/server-filesystem/dist/index.js",
          "/home/derickpayne/impossible-os"
      ]
  }
  ```
- [x] Server appears in Antigravity MCP panel ✅
- [x] Tools available:
  - `read_text_file` — read file contents (supports head/tail)
  - `read_media_file` — read images/audio as base64
  - `read_multiple_files` — batch read multiple files
  - `write_file` — create or overwrite files
  - `edit_file` — selective edits with pattern matching and diff preview
  - `create_directory` — create directories (with parents)
  - `list_directory` — list files/dirs with `[FILE]`/`[DIR]` prefixes
  - `list_directory_with_sizes` — list with file sizes and sorting
  - `directory_tree` — recursive JSON tree view
  - `move_file` — move or rename files/directories
  - `search_files` — recursive file search by glob pattern
  - `get_file_info` — file metadata (size, timestamps, permissions)
  - `list_allowed_directories` — show configured sandboxed paths

---

## 3. Full MCP Configuration ✅

All three MCP servers active in `~/.gemini/antigravity/mcp_config.json`:

```json
{
    "mcpServers": {
        "srclight": {
            "command": "srclight",
            "args": [
                "serve",
                "--transport",
                "stdio",
                "--workspace",
                "dev-workspace"
            ]
        },
        "memory": {
            "command": "/usr/bin/node",
            "args": [
                "/home/derickpayne/mcp-servers/node_modules/@modelcontextprotocol/server-memory/dist/index.js"
            ],
            "env": {
                "MEMORY_FILE_PATH": "/home/derickpayne/impossible-os/.memory/memory.jsonl"
            }
        },
        "filesystem": {
            "command": "/usr/bin/node",
            "args": [
                "/home/derickpayne/mcp-servers/node_modules/@modelcontextprotocol/server-filesystem/dist/index.js",
                "/home/derickpayne/impossible-os"
            ]
        }
    }
}
```

| Server | Transport | Purpose | Status |
|---|---|---|---|
| `srclight` | stdio | AST-aware code intelligence (Tree-sitter + FTS5) | ✅ Active |
| `memory` | stdio | Persistent knowledge graph (entities/relations/observations) | ✅ Active |
| `filesystem` | stdio | Sandboxed file access (read/write/edit/search) | ✅ Active |

> [!NOTE]
> **Gotchas log (2026-03-17):**
> - `npx` in WSL PATH resolves to Windows Node.js v24 → **breaks MCP SDK** (zod ESM import error)
> - Fix: install packages locally in WSL, use `/usr/bin/node` with absolute `dist/index.js` paths
> - Windows project path: `\\wsl.localhost\Ubuntu\home\derickpayne\impossible-os`
> - WSL npm installed separately from Node.js: `sudo apt install npm` (not included with `nodejs` package)
> - Memory file stored at `/home/derickpayne/impossible-os/.memory/memory.jsonl` (gitignored)
> - MCP packages installed at `/home/derickpayne/mcp-servers/node_modules/@modelcontextprotocol/`
