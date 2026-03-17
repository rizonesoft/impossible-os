# TODO-004 — MCP Server Setup

> **Goal:** Install and configure two additional MCP servers for Antigravity:
> - **mjm.local.docs** — semantic document search for hardware specs and project docs
> - **@modelcontextprotocol/server-memory** — persistent knowledge graph memory for agents
>
> → XREF: `TODO-003-Antigravity.md §5` — MCP server ecosystem (Srclight already configured)

---

## 1. mjm.local.docs — Local Document Search Server

> [!NOTE]
> **What:** Self-hosted semantic document search with MCP interface.
> Agents can search, read, and manage documentation via `search_docs`, `add_document`, etc.
> Built with .NET 10, runs locally, supports PDF/DOCX/Markdown/TXT ingestion.
> **GitHub:** https://github.com/markjackmilian/mjm.local.docs
> **License:** MIT

### 1.1 Prerequisites

- [ ] Verify .NET 10 SDK is installed: `dotnet --version` → must show `10.x`
- [ ] If not installed: `sudo apt-get install -y dotnet-sdk-10.0`
- [ ] Verify git is available: `git --version`

### 1.2 Clone and Build

- [ ] Clone the repository:
  ```bash
  cd ~/tools
  git clone https://github.com/markjackmilian/mjm.local.docs.git
  cd mjm.local.docs
  ```
- [ ] Build the project:
  ```bash
  dotnet build src/Mjm.LocalDocs.Server/Mjm.LocalDocs.Server.csproj
  ```
- [ ] Verify build succeeds with no errors

### 1.3 Configure Embedding Provider

> [!IMPORTANT]
> The default `Fake` embedding provider is usable for testing but provides no real
> semantic search. For production use, configure one of the real providers below.

- [ ] Choose embedding provider:
  - **Fake** (default) — no API key needed, no real semantic search
  - **Ollama** — free, local, private (recommended for offline use)
  - **OpenAI** — `text-embedding-3-small`, requires `OPENAI_API_KEY`
- [ ] Edit `src/Mjm.LocalDocs.Server/appsettings.json`:
  ```json
  {
    "ConnectionStrings": {
      "LocalDocs": "Data Source=localdocs.db"
    },
    "LocalDocs": {
      "Embeddings": {
        "Provider": "Ollama",
        "Dimension": 768
      },
      "Storage": {
        "Provider": "Sqlite"
      }
    }
  }
  ```
- [ ] If using OpenAI: set environment variable `OPENAI_API_KEY`
- [ ] If using Ollama: ensure Ollama is running locally (`ollama serve`)

### 1.4 Start the Server

- [ ] Run the server:
  ```bash
  dotnet run --project src/Mjm.LocalDocs.Server/Mjm.LocalDocs.Server.csproj
  ```
- [ ] Verify web UI loads at: http://localhost:5024
- [ ] Verify MCP endpoint responds at: http://localhost:5024/mcp

### 1.5 Ingest Impossible OS Specs

- [ ] Upload spec documents via web UI or MCP tools:
  - `specs/pci-3.0.md` — PCI Local Bus Specification
  - `specs/ahci-1.3.1.md` — AHCI Specification
  - (any future specs added to `specs/` directory)
- [ ] Verify: search for "AHCI command list structure" returns relevant results
- [ ] Verify: search for "PCI configuration space" returns relevant results

### 1.6 Configure Antigravity MCP

- [ ] Add to Antigravity's MCP config (Agent Manager → MCP Servers → View raw config):
  ```json
  {
    "mcpServers": {
      "local-docs": {
        "type": "http",
        "url": "http://localhost:5024/mcp"
      }
    }
  }
  ```
- [ ] Save and refresh MCP panel
- [ ] Verify: `local-docs` appears in MCP Servers list with tools:
  - `search_docs` — semantic search across documents
  - `add_document` — ingest new documents
  - `update_document` — modify existing documents
  - `list_collections` — list document collections
  - `delete_document` — remove documents

### 1.7 Create Systemd Service *(optional)*

> [!NOTE]
> For persistent background operation, set up a systemd user service
> so the server starts automatically.

- [ ] Create `~/.config/systemd/user/mjm-local-docs.service`:
  ```ini
  [Unit]
  Description=mjm.local.docs MCP Server
  After=network.target

  [Service]
  Type=simple
  WorkingDirectory=%h/tools/mjm.local.docs
  ExecStart=/usr/bin/dotnet run --project src/Mjm.LocalDocs.Server/Mjm.LocalDocs.Server.csproj
  Restart=on-failure
  Environment=ASPNETCORE_URLS=http://localhost:5024

  [Install]
  WantedBy=default.target
  ```
- [ ] Enable and start: `systemctl --user enable --now mjm-local-docs`
- [ ] Verify: `systemctl --user status mjm-local-docs` shows active

- [ ] Commit: `"agent: configure mjm.local.docs MCP server"`

---

## 2. @modelcontextprotocol/server-memory — Knowledge Graph Memory

> [!NOTE]
> **What:** Persistent knowledge graph that stores entities, relations, and observations.
> Agents use this to remember facts across conversations — project decisions, user
> preferences, architectural patterns, debugging context, etc.
> **GitHub:** https://github.com/modelcontextprotocol/servers/tree/main/src/memory
> **Transport:** stdio (npx)
> **Storage:** JSONL file (configurable path)

### 2.1 Prerequisites

- [ ] Verify Node.js is installed: `node --version` → must show `v18+`
- [ ] If not installed: `sudo apt-get install -y nodejs npm`
- [ ] Verify npx is available: `npx --version`

### 2.2 Test Server Launch

- [ ] Verify the server runs:
  ```bash
  npx -y @modelcontextprotocol/server-memory --help
  ```
- [ ] Confirm no npm errors or missing dependencies

### 2.3 Choose Memory File Location

- [ ] Decide where to store the knowledge graph:
  - **Project-local:** `~/impossible-os/.memory/memory.jsonl` (gitignored, project-specific)
  - **Global:** `~/.mcp/memory.jsonl` (shared across all projects)
- [ ] Create the directory:
  ```bash
  mkdir -p ~/impossible-os/.memory
  ```
- [ ] Add `.memory/` to `.gitignore` (if using project-local)

### 2.4 Configure Antigravity MCP

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

### 2.5 Test Memory Operations

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

### 2.6 Seed Initial Knowledge *(optional)*

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

## 3. Verification — All MCP Servers Active

- [ ] Verify all 3 MCP servers appear in Antigravity:
  | Server | Transport | Status |
  |---|---|---|
  | `srclight` | stdio | ✅ Active |
  | `local-docs` | HTTP | ✅ Active |
  | `memory` | stdio | ✅ Active |
- [ ] Test combined workflow:
  1. Agent searches docs via `local-docs` → finds AHCI spec
  2. Agent stores finding in `memory` → creates entity
  3. Agent searches code via `srclight` → finds AHCI driver implementation
- [ ] Commit: `"agent: verify all MCP servers operational"`
