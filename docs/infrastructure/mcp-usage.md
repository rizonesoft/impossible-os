# MCP Usage Discipline

> Canonical guide for when Claude (and Codex, post-Codex-MCP-wiring) should call the project's two MCP servers vs. the built-in Read / Edit / Bash(grep) tools. Written 2026-04-27 in response to the [Automation Hardening MCP Usage Discipline](../../todo/00-infrastructure/TODO-08-automation-hardening.md#6-mcp-usage-discipline----doc--claudemd-pointer) effort: the observed pattern of "Claude greps for symbols when `lsp-bridge.definition` would be one call; Claude walks TODO XREFs by hand instead of calling `todo-graph.backlinks`".

## Inventory

Two MCP servers ship with this repo, both `Read`-only over their respective caches:

| Server       | Owner                                                                                                                                                                               | Source                                                                       | Cache layer                                                                                         |
| ------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------- |
| `todo-graph` | [TODO Metadata Layer -- MCP Server (AI-Agent Transport over the Cache)](../../todo/00-infrastructure/TODO-06-todo-metadata-layer.md#8-mcp-server-ai-agent-transport-over-the-cache) | [`scripts/todo-graph/mcp_server.py`](../../scripts/todo-graph/mcp_server.py) | `build/todo-cache.json` (auto-rebuilt when stale)                                                   |
| `lsp-bridge` | [LSP-MCP Bridge](../../todo/00-infrastructure/TODO-07-lsp-mcp-bridge.md)                                                                                                            | [`scripts/lsp-mcp/bridge.py`](../../scripts/lsp-mcp/bridge.py)               | per-language LSP daemons (clangd, asm-lsp, bash-language-server, pyright, PowerShellEditorServices) |

Both servers are wired in [`.mcp.json`](../../.mcp.json) at project scope; Claude Code loads them at session start. Codex CLI gains the same wiring once the [Codex MCP wiring + cross-config drift validator](../../todo/00-infrastructure/TODO-08-automation-hardening.md#1-codex-mcp-wiring--cross-config-drift-validator) work ships.

### Headless availability caveat (measured 2026-07-03)

This guide is written for INTERACTIVE sessions, where both servers reliably connect and their `mcp__*` tools are in the model's tool list. Under **headless `claude -p`** (the overnight runner) the story is different and load-bearing:

- The servers connect ASYNCHRONOUSLY (~0.7s stdio handshake each -- neither is slow), and `-p` does NOT block session initialization on that connection. There is no knob to force it (`MCP_TIMEOUT` was tested and has no effect on the init snapshot).
- On a COLD start -- every launcher spawn and every watchdog relaunch -- the init tool-list snapshot catches the servers still `pending` roughly **2 of 3 times** (measured on the WSL2 dev host). When that happens the session begins with **zero `mcp__` tools** and the model cannot call them for the rest of the run. A 20 h production run made zero MCP calls for exactly this reason.
- Approval is NOT the gate (`hasTrustDialogAccepted: true` at the project level suffices; an empty `enabledMcpjsonServers` still loads them). Account-level claude.ai MCP servers, when present, add a dozen more connections competing for the same startup window.

**Headless rule:** when the `mcp__` tools are absent, do NOT emulate them with manual `grep -n "^## N\."` section scans or `sed -n 'X,Yp'` reads. The MCP servers are thin wrappers over deterministic, always-available CLIs -- use those instead: `python3 scripts/todo-graph/query.py <verb>` (backlinks / ready / deferred-by / code / ...) and [`scripts/todo-graph/resolve_symbol.py`](../../scripts/todo-graph/resolve_symbol.py), plus the built-in **Grep / Glob** tools (never Bash `grep`/`sed`) and slice `Read(offset, limit)`. The Grep-tool-over-Bash-grep floor is MCP-independent and always applies.

lsp-bridge health note: the OPTIONAL per-language LSP binaries (pyright, asm-lsp, PSES, ...) are declared OPTIONAL-tier in `setup.sh` -- when one is not installed, the bridge logs it at INFO (`warm-skipped-not-installed`, status `not-installed`) rather than a WARN `restart-failed`, and the query path skips it cleanly (fix 2026-07-03). pyright is per-file by design (settled 2026-07-12, see [pyright Integration (Python)](../../todo/00-infrastructure/TODO-07-lsp-mcp-bridge.md#5-pyright-integration-python) follow-up): with `pyright-langserver` installed (`npm install -g pyright`), per-file py analysis works (hover / definition / references / `documentSymbol`) but `workspace/symbol` returns empty on this repo -- open-source pyright has NO cross-file workspace-symbol index (that background indexing is Pylance-only; `python.analysis.indexing` does not exist in open-source pyright), so no config can enable it. The bridge treats pyright as a per-file engine (dropped `workspaceSymbolProvider` from its required caps; the py smoke exercises `documentSymbol`). Cross-file py symbol lookup uses the deterministic fallback (`scripts/todo-graph/resolve_symbol.py` + ripgrep). Swapping to an indexing Python LSP is a candidate in [TODO-07](../../todo/00-infrastructure/TODO-07-lsp-mcp-bridge.md) if cross-file py intelligence becomes load-bearing.

## When to call `todo-graph`

15 read-only queries; all return JSON over stdio MCP transport. Pick by the question being asked.

**The first three are FILE-level and the corpus does not author their edge set.** They rank against the frontmatter `depends_on` key, which 0 of 232 live TODOs set, so they return empty for every possible corpus -- an empty answer from them describes the absence of a graph, not the absence of blockers. For "what should I work on next?" use the `section-*` trio, which ranks over the 715 cross-file section dependencies the Implementation Order tables do author. Full rule: [Section-level readiness: the aggregation rule](todo-metadata.md#section-level-readiness-the-aggregation-rule).

| Tool | Use when |
|---|---|
| `mcp__todo-graph__section-ready` | **"What should I work on next?"** -- open sections whose cross-file AND same-file section dependencies are all satisfied. |
| `mcp__todo-graph__section-blocked` | "What is section X waiting on?" -- each unmet dependency named with why (`open` / `blocked` / `file-open` / `dangling` / `non-node` / `unresolved`). |
| `mcp__todo-graph__section-blocking` | "What is most worth finishing?" -- target sections ranked by how many open sections they hold up. The real critical-path rank. |
| `mcp__todo-graph__ready` | File-level: TODOs whose frontmatter `depends_on` are all done. Empty until a TODO authors the field. |
| `mcp__todo-graph__blocked` | File-level: TODOs with an unmet frontmatter `depends_on`. Empty until a TODO authors the field. |
| `mcp__todo-graph__blocking` | File-level: rank by inbound frontmatter `depends_on`. Empty until a TODO authors the field. |
| `mcp__todo-graph__by-domain <domain>` | "What TODOs live in domain `01-boot-platform`?" -- domain inventory. |
| `mcp__todo-graph__backlinks <id>` | "Who XREFs TODO Y?" -- walks every inbound XREF / depends_on / satisfies / Inputs / Accepted-stamp / Deferred-stamp pointing at the target. **The fast path for cross-TODO impact analysis.** |
| `mcp__todo-graph__deferred <id>` | "What does TODO Y defer to elsewhere?" -- outbound `Deferred:` stamps. |
| `mcp__todo-graph__deferred-by <id>` | "Who defers work to TODO Y?" -- inbound `Deferred:` + `Accepted:` stamps targeting Y. |
| `mcp__todo-graph__orphans` | "Are there TODOs nobody references?" -- governance / cleanup signal. |
| `mcp__todo-graph__stale [days]` | "What TODO sections haven't been touched in N days?" -- defaults 90. |
| `mcp__todo-graph__stats` | "How many TODOs / sections / open items / by-domain breakdown?" -- repo health snapshot. |
| `mcp__todo-graph__code <id>` | "What source files does TODO Y own / produce?" -- maps an `id` to `path:symbol` artifacts in the cache. |
| `mcp__todo-graph__code-by <path>` | "Which TODO owns `src/kernel/foo.c`?" -- inverse of `code`. |

Cache is auto-rebuilt on read when stale (mtime check vs. `todo/`). For a manual refresh: `bash scripts/todo-graph/build-and-validate.sh --keep-cache`.

**Every row-returning tool is output-bounded** (`stats` is exempt: its shape is O(taxonomy), not O(nodes)). Each response carries `returned`, `total_matching` and `truncated`, so a page can never be read as the complete set, plus a `next` string when more rows exist. Narrow with `limit` (default 50, hard cap 100 -- values outside the range are clamped, and an unbounded set is NOT requestable through MCP), `offset`, `scope` (one domain), and `fields` (column subset -- the cheapest lever: `fields=id` cut a 98-row `backlinks` answer from ~3.2k to ~0.9k estimated tokens with no loss). Ordering is total, so a bounded page is re-runnable and pages partition cleanly. `deferred` / `deferred-by` sort **severity-first**, so a truncated page cannot drop a Critical/High while keeping a Medium.

If a response exceeds the byte ceiling it **fails closed**: zero rows, plus `total_matching` and the exact `narrow_with` flags. That envelope is the answer -- re-run narrowed rather than treating it as an empty result. The unbounded escape hatch (`--limit 0`) exists on the CLI only, for humans and scripts: `python3 scripts/todo-graph/query.py by-domain --limit 0`.

## When to call `lsp-bridge`

15 LSP-backed tools. The 6 high-value ones for code intelligence:

| Tool | Use when |
|---|---|
| `mcp__lsp-bridge__definition` | "Where is `foo` defined?" -- requires `path`, `line`, `character` (cursor position). Resolve a use site via `workspace_symbol` first if you only have the symbol name. **Prefer over** `grep -nE '^.*foo.*\('` for symbol lookups once the cursor is known. Returns precise file:line + signature. |
| `mcp__lsp-bridge__references` | "Who calls / uses `foo`?" -- requires `path`, `line`, `character` (cursor position) -- this is NOT a name-based lookup. Resolve a declaration site via `workspace_symbol` (or grep one known caller) first, then call `references` with that cursor. **Prefer over** `grep -rn 'foo(' src/` once the cursor is known. Returns AST-resolved call-graph edges. |
| `mcp__lsp-bridge__hover` | "What's the type / docstring of `foo`?" -- requires `path`, `line`, `character`. One-shot type inspection without opening the file. |
| `mcp__lsp-bridge__workspace_symbol <query>` | "Find all symbols matching pattern X across the workspace." -- fuzzy-match by name across all parsed languages. **The standard resolver before `definition` / `references` / `hover` when you only have a name.** |
| `mcp__lsp-bridge__document_symbol <path>` | "What symbols does file `<path>` define?" -- per-file outline. |
| `mcp__lsp-bridge__diagnostics <path>` | "What does the LSP think is broken in `<path>`?" -- compiler-grade unused-include / dead-code / type-error reports. `path` is required (no whole-workspace shortcut today). |

Specialized (less common):
`completion`, `signature_help`, `type_definition`, `implementation`, `declaration`, `call_hierarchy_incoming`, `call_hierarchy_outgoing`, `code_action`, `_health`.

**Discipline rules:**

1. Name-to-cursor resolution: when you only have a symbol name (no file:line), call `workspace_symbol` first to get the canonical declaration site, then feed the resulting `path/line/character` into `definition` / `references` / `hover`. Skipping the resolver step is the most common cause of MCP calls that fail with missing-required-arg errors.
2. Symbol queries: once you have a cursor, **prefer `definition` over `grep '^.*foo.*('`**. The LSP returns one canonical answer; the grep returns N partial regex matches that need disambiguation.
3. Caller queries: once you have a cursor, **prefer `references` over `grep -rn 'foo('`**. The LSP traverses the AST; the grep matches text in comments, docstrings, and shadowed scopes.
4. Cross-file consistency audits: **resolve cursor via `workspace_symbol`, then call `references`**, then dispatch the Codex review with the reference set inline (so Codex sees actual callers, not what it heuristically thinks they are).
5. Type / doc lookup: **prefer `hover` over `Read` + `grep`** when you don't need the file body.

## When NOT to call MCP

Stay on built-in tools for:

| Use case                                                        | Tool                                                                                                               |
| --------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| Reading a file's actual contents                                | `Read` (NOT `lsp-bridge` -- it returns symbol metadata, not bytes)                                                 |
| Editing a file                                                  | `Edit` / `Write`                                                                                                   |
| Markdown body text search ("find every TODO mentioning 'shim'") | `Bash(grep -rn ...)` -- LSP doesn't index markdown bodies; `todo-graph` indexes structural metadata, not free text |
| Searching `scripts/` shell / Python helpers for a string        | `Bash(grep ...)` -- LSP coverage is partial; grep is honest                                                        |
| Build-artifact inspection (`build/*.log`, generated headers)    | `Read` / `Bash(grep ...)` -- not in any MCP cache                                                                  |
| Git operations (log, diff, blame, show)                         | `Bash(git ...)` -- no MCP wraps git                                                                                |
| Running tests / scripts                                         | `Bash`                                                                                                             |

A useful heuristic: **if the answer is "look at the actual file", use `Read`. If the answer is "look up structural metadata", use the appropriate MCP. If the answer is "find this string anywhere", use `grep`.**

## Cross-tool parity (post Codex MCP wiring)

After the [Codex MCP wiring](../../todo/00-infrastructure/TODO-08-automation-hardening.md#1-codex-mcp-wiring--cross-config-drift-validator) work ships, Codex CLI loads the same `[mcp_servers.todo-graph]` and `[mcp_servers.lsp-bridge]` blocks via `~/.codex/config.toml`, so a Codex review pass has access to the same dependency-graph + LSP-grade code intelligence Claude has. Today (pre-wiring), Codex review is cold-read of file paths in the prompt -- the MCP-driven context is Claude-only, which is why every Codex dispatch reads files in full to compensate. Once the wiring ships, drift between the two automation sides closes.

The [Cross-Tool Drift Detection](../../todo/00-infrastructure/TODO-08-automation-hardening.md#9-cross-tool-drift-detection----scriptsaudit-ai-systemsh) work audits that both `.mcp.json` and `~/.codex/config.toml` enumerate the same server set; CI fails on drift.
