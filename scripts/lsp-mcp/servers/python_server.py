#!/usr/bin/env python3
# ============================================================================
# scripts/lsp-mcp/servers/python_server.py -- pyright (Python) integration.
#
# Owner: TODO-07 pyright Integration (Python) in 00-infrastructure.
#
# Spawns Microsoft pyright-langserver for the ~6.4k lines of Python under
# scripts/todo-graph/, tools/, and the lsp-mcp bridge itself. Pyright is
# preferred over ruff-lsp because the type inference catches real contract
# bugs in our telemetry-heavy tooling that pure linting misses; ruff still
# runs via pre-commit lint, so the two cover complementary surfaces.
#
# Three design decisions worth flagging here so a future reader does not
# undo them in good faith:
#
#   1. Two-stage availability probe (PATH + --version). Pyright is shipped
#      as an npm-installed Node script -- the same packaging as bash-
#      language-server. shutil.which can return a binary that re-execs
#      into a missing or broken Node runtime; treating that as "available"
#      pushes the failure into the LSP handshake (15-second timeout, FAIL
#      banner) when the spec-correct outcome is the same SKIP path used
#      for a fully absent installation. Mirrors bash_server's probe
#      shape verbatim so the failure behavior is uniform across the two
#      Node-shim LSPs in the bridge.
#
#   2. Configuration via auto-discovery, not initializationOptions.
#      Pyright reads pyrightconfig.json first, then [tool.pyright] inside
#      pyproject.toml. The repo currently has neither (verified at
#      implementation time); pyright falls back to defaults that already
#      type-check our scripts/ tree adequately. Passing
#      cwd=workspace_root lets pyright auto-discover any future config
#      drop-in without code changes here. If a future commit pins a
#      pyright config, document the choice in the LSP-MCP developer-
#      tooling docs subsection; no python_server.py change required.
#
#   3. We do NOT advertise workspace.configuration in the client
#      capabilities. Same reason as bash_server: LspSubprocess
#      (lsp_client.py) routes responses + caches publishDiagnostics
#      notifications, but does NOT answer server-initiated requests.
#      Advertising the cap would let pyright send a workspace/
#      configuration request and block waiting for a reply we never
#      produce. Pyright degrades gracefully to its hardcoded server
#      defaults (openFilesOnly, standard type-checking) with the cap
#      absent -- it checks hasConfigurationCapability BEFORE issuing
#      the RPC, so it never even asks (verified 2026-07-12 against
#      pyright languageServerBase.ts getConfiguration()). This does
#      NOT affect workspace/symbol: pyright's cross-file symbol
#      indexing is a Pylance-only feature that open-source pyright
#      simply lacks, so no configuration value could turn it on. See
#      required_capabilities() for why workspaceSymbolProvider is
#      dropped and py is treated as a per-file engine.
#      Reverse-RPC handling is a bridge-skeleton retrofit if a future
#      server requires dynamic config reads.
#
# Graceful-skip contract mirrors clangd_server / asm_server / bash_server:
# is_available() returns False when pyright-langserver is absent OR present
# but the Node shim is broken; spawn() raises LspError with kind
# 'lsp-binary-missing', which the bridge's SKIP path surfaces as exit-0
# output. CI hosts without pyright stay green.
# ============================================================================

from __future__ import annotations

import functools
import shutil
import subprocess
from pathlib import Path
from typing import Any

from lsp_client import (LspError, LspSubprocess,
                        report_unconfirmed_shutdown)


PYRIGHT_BIN = "pyright-langserver"
PYRIGHT_CLI_BIN = "pyright"
LANG_TAG = "py"

# Pyright-langserver is shipped as an npm-installed Node script. The
# wrapper on PATH can be present while its Node runtime is missing or
# broken -- shutil.which cannot detect this. The pre-flight probe below
# catches that case so the SKIP path stays uniform with "binary truly
# absent" instead of falling into a confusing FAIL during the LSP
# handshake.
#
# Implementation note: pyright-langserver itself does NOT support a
# --version flag (it exits with a connection-required error). The npm
# `pyright` package always ships BOTH binaries side-by-side -- the LSP
# server (`pyright-langserver`) and the CLI type-checker (`pyright`).
# We probe the sibling CLI binary because (a) it does support --version
# and exits 0, and (b) if one of the two is a broken Node shim, both
# are: they share the same package install. This is a tighter contract
# than bash_server's self-probe but the same dual-stage spirit -- PATH
# presence is necessary but not sufficient for npm-shim LSPs.
_VERSION_PROBE_TIMEOUT_S = 3.0


@functools.lru_cache(maxsize=1)
def is_available() -> bool:
    """True iff pyright-langserver can actually be invoked. Two-stage
    check: BOTH the langserver binary AND its sibling `pyright` CLI
    are on PATH, AND a `pyright --version` probe completes successfully
    within a few seconds.

    Why probe the CLI binary instead of the langserver: pyright-
    langserver expects LSP connection arguments and exits non-zero on
    --version (the documented failure mode is "Connection input stream
    is not set"). The npm pyright package installs both `pyright` and
    `pyright-langserver` shims into the same node_modules/.bin
    directory; if the Node runtime under one shim is broken, it is
    broken under the other. So `pyright --version` is a sound proxy
    for "this pyright install is runnable".

    The result is memoized for the lifetime of the process. The bridge
    self-test gate calls this once before _get_or_spawn(), and spawn()
    calls it again inside the spawner; without the cache, both calls
    would pay full Node startup cost. Within a single bridge process
    the answer cannot change (a host that gains or loses pyright mid-
    process is not a case the bridge supports), so a one-shot cache
    is sound."""
    if shutil.which(PYRIGHT_BIN) is None:
        return False
    if shutil.which(PYRIGHT_CLI_BIN) is None:
        # Should not happen with a normal npm install, but a hand-
        # placed langserver binary without the CLI sibling is not a
        # supported configuration -- treat it as unavailable rather
        # than skipping the runtime probe entirely.
        return False
    try:
        result = subprocess.run(
            [PYRIGHT_CLI_BIN, "--version"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=_VERSION_PROBE_TIMEOUT_S,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return result.returncode == 0


def install_hint() -> str:
    """Single source of truth for the SKIP message + install guidance.
    Pyright is published on npm; the canonical install command is
    npm-based. Same shape as bash_server's hint so the MCP envelope
    stays uniform across npm-shim LSPs."""
    return "npm install -g pyright (requires node >= 14)"


def spawn(workspace_root: Path) -> LspSubprocess:
    """Spawn pyright-langserver and complete the initialize handshake.

    Returns a fully-initialized LspSubprocess with server_caps populated.
    Raises LspError('lsp-binary-missing') when pyright-langserver is
    not installed; callers translate that into the JSON error envelope
    (or SKIP in the bridge self-test).

    The server is invoked with `pyright-langserver --stdio` -- the
    `--stdio` flag selects the LSP-over-stdio transport (other modes
    exist for socket transports, which we do not use). cwd is set to
    the workspace root so pyright auto-discovers any pyrightconfig.json
    or pyproject.toml [tool.pyright] block without us having to pass
    a CLI flag.

    Capability shape advertises hover / definition / references /
    documentSymbol / workspaceSymbol / publishDiagnostics on the
    CLIENT side. We do NOT advertise workspace.configuration -- see
    the file header for why. The client-side workspace.symbol
    advertisement is kept (harmless; the generic bridge tool routes
    through it), but pyright's workspace/symbol is NOT a relied-on cap
    here: open-source pyright has no cross-file symbol index and
    returns empty in practice on this repo, so required_capabilities()
    drops workspaceSymbolProvider and the self-test smoke exercises
    documentSymbol (a per-file cap that works) instead.
    """
    if not is_available():
        raise LspError(
            "lsp-binary-missing",
            f"{PYRIGHT_BIN!r} availability probe failed (binary missing on PATH "
            "OR present but the npm shim's sibling pyright CLI failed --version)",
            lang=LANG_TAG,
            install_hint=install_hint(),
        )

    # `pyright-langserver --stdio` selects the LSP stdio transport. The
    # shim installed by `npm install -g pyright` resolves to a node-
    # launched script; if Node itself is missing the spawn fails with
    # ENOENT, which the caller surfaces via the standard subprocess-
    # spawn-failed path.
    cmd = [PYRIGHT_BIN, "--stdio"]

    lsp = LspSubprocess(cmd, lang=LANG_TAG, cwd=workspace_root)

    # Honest capability surface: only what the bridge can actually
    # route. Pyright offers all six providers below; we advertise them
    # so the tool-wiring handlers can rely on them being present.
    # publishDiagnostics is the notification path pyright uses to
    # report type errors; advertising it lets the tool-wiring commit
    # surface diagnostics through the same path bash_server uses.
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
        # Advertise server-initiated work-done progress so pyright
        # publishes its analysis progress via $/progress. Pyright
        # uses workDoneProgress/create + $/progress for background
        # type-checking; the warm-start readiness path observes
        # those notifications.
        "window": {"workDoneProgress": True},
    }

    try:
        lsp.initialize(
            root_uri=workspace_root.as_uri(),
            capabilities=capabilities,
            initialization_options=None,
            timeout=15.0,
        )
    except Exception:
        # Verdict REPORTED, not discarded: an unconfirmed death here
        # leaves a server running with the force sweep as its only
        # remaining collector (section 26).
        report_unconfirmed_shutdown(lsp, "python spawner init failure")
        raise
    return lsp


def required_capabilities() -> tuple[str, ...]:
    """Capabilities the pyright handshake MUST report back so the
    tool-wiring commit can depend on them. Pyright reliably advertises
    all four per-file providers below across every release that ships
    on npm; asserting them here means a future pyright regression that
    drops one surfaces in our self-test instead of in a user's
    hover/symbol call that silently returns null.

    workspaceSymbolProvider is NOT in this tuple (same disposition as
    bash_server, for a different reason). Open-source pyright DOES
    advertise workspaceSymbolProvider in the handshake, but its
    workspace/symbol only searches files already tracked by the
    program (open files); it has no persisted workspace index -- that
    background-indexing feature is Pylance-only and does not exist in
    open-source pyright-langserver (verified 2026-07-12 against
    microsoft/pyright workspaceSymbolProvider.ts + docs/settings.md:
    no `python.analysis.indexing`, no on-disk `indexing` key). On this
    repo the query returns empty in practice, so we do NOT assert the
    cap (advertised != functional) and treat pyright as a per-file
    engine (hover / definition / references / documentSymbol). Root
    cause of the empty result is left for TODO-07 to chase with a live
    LSP wire trace; the `initialized` notification is already sent
    (lsp_client.py), so the common client-side cause is ruled out.
    Cross-file py symbol lookup uses the deterministic fallback
    (scripts/todo-graph/resolve_symbol.py + ripgrep).

    publishDiagnostics is NOT in this tuple because it is a
    notification direction, not a server capability key. Diagnostic
    delivery is verified separately by polling
    lsp.diagnostics_by_uri when a section needs it."""
    return (
        "hoverProvider",
        "definitionProvider",
        "referencesProvider",
        "documentSymbolProvider",
    )
