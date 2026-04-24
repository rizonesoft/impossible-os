# scripts/lsp-mcp/servers -- per-language LSP spawn recipes.
#
# Each module defines `spawn(workspace_root: Path) -> LspSubprocess`
# and is registered into bridge._LSP_SPAWNERS by bridge.py at import
# time. Keeping the modules isolated (one file per LSP) means missing
# optional deps only break their own language, not the whole bridge.
