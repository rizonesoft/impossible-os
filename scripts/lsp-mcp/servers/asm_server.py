#!/usr/bin/env python3
# ============================================================================
# scripts/lsp-mcp/servers/asm_server.py -- asm-lsp (NASM x86-64) integration.
#
# Owner: TODO-07 asm-lsp Integration (NASM) in 00-infrastructure.
#
# Spawns bergercookie/asm-lsp with two pieces of Impossible-OS-specific
# wiring:
#
#   1. Repo-root `.asm-lsp.toml` pins `assembler = "nasm"` +
#      `instruction_set = "x86-64"` (hyphenated; the upstream
#      asm-lsp schema rejects the underscore form). asm-lsp
#      defaults to GAS syntax;
#      without the pin, every mov/jmp/label/macro in our .asm files
#      parses wrong and diagnostics become pure noise.
#
#   2. Extensions `.asm` + `.S`. The bulk of the tree uses `.asm`
#      (src/boot/entry.asm, src/kernel/isr_stubs.asm, etc.); some
#      future bootloader TUs may use `.S` so we reserve both.
#
# Graceful-skip contract mirrors clangd_server: `is_available()`
# returns False when `asm-lsp` is absent; `spawn()` raises LspError
# with kind `lsp-binary-missing`, which the bridge's SKIP path
# surfaces as exit-0 output. CI hosts without asm-lsp stay green.
# ============================================================================

from __future__ import annotations

import shutil
from pathlib import Path
from typing import Any

from lsp_client import LspError, LspSubprocess


ASM_LSP_BIN = "asm-lsp"
LANG_TAG = "asm"


def is_available() -> bool:
    """True iff the asm-lsp binary is on PATH. Used by the bridge's
    --self-test to decide SKIP vs spawn."""
    return shutil.which(ASM_LSP_BIN) is not None


def install_hint() -> str:
    """Single source of truth for the SKIP message + install guidance.
    asm-lsp is a Rust crate published on crates.io; the canonical
    install command is cargo-based. Same shape as clangd_server's
    install_hint so the MCP envelope stays uniform across languages."""
    return "cargo install asm-lsp (requires a working Rust toolchain)"


def spawn(workspace_root: Path) -> LspSubprocess:
    """Spawn asm-lsp and complete the initialize handshake.

    Returns a fully-initialized LspSubprocess with server_caps
    populated. Raises LspError('lsp-binary-missing') when asm-lsp
    is not installed; callers translate that into the JSON error
    envelope (or SKIP in the bridge self-test).

    The NASM pin lives in `.asm-lsp.toml` at the workspace root;
    asm-lsp auto-discovers it from cwd, so we pass `cwd=workspace_root`
    rather than a CLI flag. That keeps the binary invocation minimal
    and matches how human developer hosts (VS Code, Neovim) drive
    asm-lsp for this repo.

    Handshake uses the SAME advertised capability shape as
    clangd_server (textDocument.synchronization / hover / definition /
    references / documentSymbol / publishDiagnostics +
    workspace.symbol). The reader loop in LspSubprocess routes only
    response-to-our-request + publishDiagnostics notifications;
    workspace.configuration / workspaceFolders are deliberately NOT
    advertised -- server-initiated-request handling lands with the
    file-change-lifecycle + workspace-symbol tool commits.
    """
    if not is_available():
        raise LspError(
            "lsp-binary-missing",
            f"{ASM_LSP_BIN!r} not found on PATH",
            lang=LANG_TAG,
            install_hint=install_hint(),
        )

    # asm-lsp reads .asm-lsp.toml from cwd automatically; no CLI flag.
    cmd = [ASM_LSP_BIN]

    lsp = LspSubprocess(cmd, lang=LANG_TAG, cwd=workspace_root)

    capabilities: dict[str, Any] = {
        "textDocument": {
            "synchronization": {"didSave": True, "dynamicRegistration": False},
            "hover": {"contentFormat": ["markdown", "plaintext"]},
            "definition": {"linkSupport": False},
            "references": {},
            "documentSymbol": {"hierarchicalDocumentSymbolSupport": True},
            "publishDiagnostics": {"relatedInformation": True},
        },
        "workspace": {"symbol": {}},
    }
    # asm-lsp does not accept initializationOptions the way clangd
    # does -- its runtime config lives entirely in .asm-lsp.toml. Pass
    # None so the handshake stays minimal.
    try:
        lsp.initialize(
            root_uri=workspace_root.as_uri(),
            capabilities=capabilities,
            initialization_options=None,
            timeout=10.0,
        )
    except Exception:
        try:
            lsp.shutdown(timeout=1.0)
        except Exception:
            pass
        raise
    return lsp


def required_capabilities() -> tuple[str, ...]:
    """Capabilities the asm-lsp handshake MUST report back so the
    tool-wiring commit can depend on them. asm-lsp today advertises
    the providers below; we assert them explicitly so a future
    asm-lsp regression that drops one surfaces in our self-test
    instead of in a user's hover call that silently returns null.

    Note: asm-lsp's instruction-reference payload is returned via
    textDocument/hover, not a separate method, so hoverProvider is
    the load-bearing capability for the NASM smoke path."""
    return (
        "hoverProvider",
        "definitionProvider",
        "referencesProvider",
        "documentSymbolProvider",
    )
