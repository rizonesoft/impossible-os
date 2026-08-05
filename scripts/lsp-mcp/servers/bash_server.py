#!/usr/bin/env python3
# ============================================================================
# scripts/lsp-mcp/servers/bash_server.py -- bash-language-server (.sh) integration.
#
# Owner: TODO-07 bash-language-server Integration in 00-infrastructure.
#
# Spawns mads-hartmann/bash-language-server (Node) for the ~11.4k lines of
# shell under scripts/. bash-language-server delegates real diagnostics to
# shellcheck whenever the binary is discoverable; without shellcheck the
# server falls back to its own basic syntax + style checks. Both modes
# satisfy the "diagnostics array, possibly empty" contract the smoke test
# enforces.
#
# Two design decisions worth flagging here so a future reader does not
# undo them in good faith:
#
#   1. We do NOT advertise workspace.configuration in the client
#      capabilities. Per LSP 3.17, the server sends workspace/configuration
#      reverse-RPC requests only if the client opts in. Our LspSubprocess
#      reader (lsp_client.py) routes responses + caches publishDiagnostics
#      notifications, but does NOT answer server-initiated requests. If we
#      advertised the cap, bash-language-server would send a
#      workspace/configuration request and block waiting for a reply we
#      never produce. Keeping the cap unadvertised is the spec-correct way
#      to avoid that hang -- the server uses initializationOptions + env
#      vars + defaults instead. Reverse-RPC handling is a bridge-skeleton
#      retrofit if a future server (pyright, PSES) requires dynamic config
#      reads.
#
#   2. shellcheck wiring goes through initializationOptions, not
#      workspace/configuration. bash-language-server documents
#      shellcheckPath / shellcheckArguments under initializationOptions for
#      clients that do not implement dynamic configuration. An empty
#      shellcheckPath ("") asks the server to auto-discover shellcheck on
#      PATH, which is the documented default. CI hosts without shellcheck
#      get bash-language-server's built-in linter; dev hosts with
#      shellcheck on PATH get the real diagnostics.
#
# Graceful-skip contract mirrors clangd_server / asm_server: is_available()
# returns False when bash-language-server is absent; spawn() raises LspError
# with kind 'lsp-binary-missing', which the bridge's SKIP path surfaces as
# exit-0 output. CI hosts without bash-language-server stay green.
# ============================================================================

from __future__ import annotations

import functools
import shutil
import subprocess
from pathlib import Path
from typing import Any

from lsp_client import (LspError, LspSubprocess,
                        report_unconfirmed_shutdown)


BASH_LSP_BIN = "bash-language-server"
SHELLCHECK_BIN = "shellcheck"
LANG_TAG = "sh"

# bash-language-server is shipped as an npm-installed Node script. Unlike
# clangd-19 (native binary) or asm-lsp (Rust binary), the wrapper on PATH
# can be present while its Node runtime is missing or broken -- shutil.which
# cannot detect this. The pre-flight --version probe below catches that
# case so the SKIP path stays uniform with "binary truly absent" instead of
# falling into a confusing FAIL during the LSP handshake.
_VERSION_PROBE_TIMEOUT_S = 3.0


@functools.lru_cache(maxsize=1)
def is_available() -> bool:
    """True iff bash-language-server can actually be invoked. Two-stage
    check: the binary is on PATH AND a `--version` probe completes
    successfully within a few seconds.

    The probe matters because `bash-language-server` is an npm shim that
    re-exec's into Node. If Node is missing or the shim is corrupted, the
    binary appears present but every spawn fails. Treating that case as
    "available" caused the LSP handshake to time out and report FAIL,
    when the spec-correct outcome is the same SKIP path used for a fully
    absent installation.

    The result is memoized for the lifetime of the process. The bridge
    self-test gate calls this once before _get_or_spawn(), and spawn()
    calls it again inside the spawner; without the cache, both calls
    would pay full Node startup cost. Within a single bridge process the
    answer cannot change (a host that gains or loses bash-language-server
    mid-process is not a case the bridge supports), so a one-shot cache
    is sound."""
    if shutil.which(BASH_LSP_BIN) is None:
        return False
    try:
        result = subprocess.run(
            [BASH_LSP_BIN, "--version"],
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
    bash-language-server is published on npm; the canonical install
    command is npm-based. shellcheck is mentioned because diagnostic
    quality drops sharply without it (bash-language-server's built-in
    checks are much narrower than shellcheck's rule set)."""
    return (
        "npm install -g bash-language-server "
        "(and apt install shellcheck for full diagnostics)"
    )


def spawn(workspace_root: Path) -> LspSubprocess:
    """Spawn bash-language-server and complete the initialize handshake.

    Returns a fully-initialized LspSubprocess with server_caps populated.
    Raises LspError('lsp-binary-missing') when bash-language-server is
    not installed; callers translate that into the JSON error envelope
    (or SKIP in the bridge self-test).

    The server is invoked with `bash-language-server start` -- the
    `start` subcommand selects the stdio LSP mode (other modes exist
    for socket transports, which we do not use). cwd is set to the
    workspace root so relative paths in shellcheck output resolve
    correctly.

    Capability shape advertises hover / definition / references /
    documentSymbol / workspaceSymbol / publishDiagnostics. We do NOT
    advertise workspace.configuration -- see the file header for why.

    initializationOptions carries the shellcheck wiring:
      * shellcheckPath: explicit path if shellcheck is on PATH, else
        empty string (which bash-language-server treats as "auto-
        discover on PATH and silently disable shellcheck if absent").
      * shellcheckArguments: empty list -- shellcheck's defaults are
        already the right baseline for our shell scripts; users who
        want to tune them set SHELLCHECK_OPTS as documented upstream.
    """
    if not is_available():
        raise LspError(
            "lsp-binary-missing",
            f"{BASH_LSP_BIN!r} availability probe failed (binary missing on PATH "
            "OR present but the npm shim's Node runtime is not runnable)",
            lang=LANG_TAG,
            install_hint=install_hint(),
        )

    # `bash-language-server start` selects stdio transport. The shim
    # installed by `npm install -g` resolves to a node-launched script;
    # if Node itself is missing the spawn fails with ENOENT, which the
    # caller surfaces via the standard subprocess-spawn-failed path.
    cmd = [BASH_LSP_BIN, "start"]

    lsp = LspSubprocess(cmd, lang=LANG_TAG, cwd=workspace_root)

    # Honest capability surface: only what the bridge can actually
    # route. bash-language-server offers all six providers below; we
    # advertise them so the tool-wiring handlers can rely on them being
    # present. publishDiagnostics is critical -- the smoke test depends
    # on the server pushing diagnostics asynchronously after didOpen.
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

    # initializationOptions: shellcheck wiring per the bash-language-
    # server README. Empty shellcheckPath means "auto-detect on PATH";
    # passing an explicit path when we have one removes the auto-detect
    # round trip and makes the configuration deterministic across hosts
    # where multiple shellcheck installs collide (e.g. brew + apt).
    shellcheck_path = shutil.which(SHELLCHECK_BIN) or ""
    initialization_options: dict[str, Any] = {
        "shellcheckPath": shellcheck_path,
        "shellcheckArguments": [],
    }

    try:
        lsp.initialize(
            root_uri=workspace_root.as_uri(),
            capabilities=capabilities,
            initialization_options=initialization_options,
            timeout=15.0,
        )
    except Exception:
        # Verdict REPORTED, not discarded: an unconfirmed death here
        # leaves a server running with the force sweep as its only
        # remaining collector (section 26).
        report_unconfirmed_shutdown(lsp, "bash spawner init failure")
        raise
    return lsp


def required_capabilities() -> tuple[str, ...]:
    """Capabilities the bash-language-server handshake MUST report back
    so the tool-wiring commit can depend on them. We assert only the
    four providers below; workspaceSymbolProvider is OMITTED on purpose.

    bash-language-server has shipped workspaceSymbolProvider on its
    main branch but the version-to-version stability is uneven --
    older releases on common LTS distros do not advertise it. Asserting
    it here would hard-fail the handshake on hosts where everything
    else works; that is the same anti-pattern asm_server avoids by
    dropping caps that asm-lsp upstream does not provide. The
    tool-wiring commit gates workspace_symbol routing on the actual
    runtime cap, so an LSP that lacks the provider degrades to a
    structured error on that one tool instead of failing to spawn at
    all.

    publishDiagnostics is NOT in this tuple because it is a notification
    direction, not a server capability key. Diagnostic delivery is
    verified by the smoke test directly polling lsp.diagnostics_by_uri."""
    return (
        "hoverProvider",
        "definitionProvider",
        "referencesProvider",
        "documentSymbolProvider",
    )
