#!/usr/bin/env python3
# ============================================================================
# scripts/lsp-mcp/servers/clangd_server.py -- clangd-19 (C / H) integration.
#
# Owner: TODO-07 clangd Integration (C / H) in 00-infrastructure.
#
# Spawns clangd-19 over stdio with two pieces of Impossible-OS-specific
# wiring that a generic clangd invocation would miss:
#
#   1. `--compile-commands-dir=<repo-root>` points clangd at the
#      Bear-generated compile_commands.json so every TU in the kernel
#      tree parses with the right flags. Without this, clangd would
#      walk upward from each source file and we cannot guarantee the
#      MCP host launches the bridge from the repo root.
#
#   2. initializationOptions.fallbackFlags forwards the freestanding
#      cross-compile flags from the repo-level `.clangd` config --
#      `--target=x86_64-elf`, `-nostdlib`, `-nostdinc`,
#      `-ffreestanding`, `-mno-red-zone`. clangd reads `.clangd`
#      itself, but only after it receives an `initialize` request;
#      passing the flags as a fallback covers the narrow window and
#      also covers TUs that are not in compile_commands.json.
#
# Graceful-skip contract: `is_available()` returns False when the
# clangd-19 binary is absent; `spawn()` raises LspError with kind
# `lsp-binary-missing` which bridge._call_lsp() then turns into the
# standard JSON error envelope. CI hosts without clangd see SKIP and
# exit 0, matching the `--self-test` discipline in mcp_server.py.
#
# Freestanding-flag drift note: the `fallbackFlags` list below MUST
# match the CompileFlags.Add block in repo-root `.clangd`. A mismatch
# makes clangd parse the kernel tree with the wrong target and
# silently report bogus diagnostics. If you edit `.clangd`, edit this
# list too.
# ============================================================================

from __future__ import annotations

import shutil
from pathlib import Path
from typing import Any

# Absolute import from the bridge package (scripts/lsp-mcp/ is on
# sys.path when bridge.py imported this module; this file works under
# both `from servers import clangd_server` and direct `python3
# scripts/lsp-mcp/servers/clangd_server.py` invocation).
from lsp_client import (LspError, LspSubprocess,
                        report_unconfirmed_shutdown)


CLANGD_BIN = "clangd-19"
LANG_TAG = "c"


# Freestanding cross-compile flags mirrored from the repo-level
# `.clangd` CompileFlags.Add block. Keep in sync; a mismatch makes
# clangd parse the kernel tree with the wrong target triple.
_FREESTANDING_FLAGS: list[str] = [
    "--target=x86_64-elf",
    "-nostdlib",
    "-nostdinc",
    "-ffreestanding",
    "-mno-red-zone",
]


def is_available() -> bool:
    """True iff the clangd-19 binary is on PATH. Used by the bridge's
    --self-test to decide SKIP vs spawn."""
    return shutil.which(CLANGD_BIN) is not None


def install_hint() -> str:
    """Single source of truth for the SKIP message + install guidance.
    The same string is surfaced in the MCP error envelope and in the
    self-test SKIP line so users get one actionable command."""
    return "apt install clangd-19 (or equivalent for your distro)"


def spawn(workspace_root: Path) -> LspSubprocess:
    """Spawn clangd-19 and complete the initialize handshake.

    Returns a fully-initialized LspSubprocess with server_caps
    populated. Raises LspError('lsp-binary-missing') when clangd-19
    is not installed; callers translate that into the JSON error
    envelope returned to MCP agents (or SKIP in the bridge self-test).

    The returned instance is owned by bridge._LIVE_LSPS; the bridge's
    shutdown path reaps it via atexit.
    """
    if not is_available():
        raise LspError(
            "lsp-binary-missing",
            f"{CLANGD_BIN!r} not found on PATH",
            lang=LANG_TAG,
            install_hint=install_hint(),
        )

    # `--background-index`: build a persistent index of workspace
    # symbols in the background so workspace/symbol queries return
    # results after a warm-up. Off by default in clangd for safety.
    # `--header-insertion=never`: MCP callers never ask clangd to
    # modify files, so header-insertion heuristics are dead weight
    # and can slow completion. Keeping them off also enforces the
    # read-only MCP surface we promise callers (no writes, no
    # workspace/applyEdit, no code-action execute): even if a later
    # tool wiring slipped, clangd would have no insertion
    # suggestions to serve.
    # `--log=error`: suppress clangd's verbose info/debug logs; the
    # stderr drain in LspSubprocess handles overflow, but keeping
    # the output small reduces drain-thread CPU on every request.
    cmd = [
        CLANGD_BIN,
        f"--compile-commands-dir={workspace_root}",
        "--background-index",
        "--header-insertion=never",
        "--log=error",
    ]

    lsp = LspSubprocess(cmd, lang=LANG_TAG, cwd=workspace_root)

    # Announce MCP-relevant capabilities so clangd advertises exactly
    # the providers our tool handlers will use. Unadvertised caps let
    # clangd skip work; it still responds on request (LSP
    # capabilities are negotiation, not hard gating), but keeping
    # the surface narrow reduces spurious server chatter.
    # Only advertise capabilities we can actually honor. Earlier
    # revisions listed workspace.configuration / workspaceFolders,
    # but the reader loop in LspSubprocess only routes responses to
    # our requests + caches publishDiagnostics notifications -- it
    # does NOT answer server-initiated requests with an `id`. If
    # clangd took us up on those caps, it would sit waiting for a
    # reply and surface as first-call latency or timeouts. The
    # server-initiated-request surface lands with the file-change
    # lifecycle + workspace-symbol tools; advertise those caps
    # then, not now.
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
        # Advertise server-initiated work-done progress so clangd
        # publishes its background-index progress via $/progress.
        # The reader thread now answers window/workDoneProgress/create
        # and observes $/progress notifications -- the warm-start
        # readiness path keys off them.
        "window": {"workDoneProgress": True},
    }
    init_options: dict[str, Any] = {
        # clangd accepts fallbackFlags via initializationOptions;
        # applied when a file is not in compile_commands.json.
        "fallbackFlags": list(_FREESTANDING_FLAGS),
    }

    try:
        lsp.initialize(
            root_uri=workspace_root.as_uri(),
            capabilities=capabilities,
            initialization_options=init_options,
            timeout=15.0,  # clangd handshake is heavier than other LSPs
        )
    except Exception:
        # Reap on handshake failure so we do not leak the subprocess
        # when the caller gets an exception.
        # Verdict REPORTED, not discarded: an unconfirmed death here
        # leaves a server running with the force sweep as its only
        # remaining collector (section 26).
        report_unconfirmed_shutdown(lsp, "clangd spawner init failure")
        raise
    return lsp


def required_capabilities() -> tuple[str, ...]:
    """Capabilities the clangd handshake MUST report back so the tool
    surface can be wired. Exported so the bridge self-test can
    verify the handshake signal without duplicating the list."""
    return (
        "hoverProvider",
        "definitionProvider",
        "referencesProvider",
        "documentSymbolProvider",
        "workspaceSymbolProvider",
    )
