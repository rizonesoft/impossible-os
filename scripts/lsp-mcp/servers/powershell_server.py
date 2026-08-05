#!/usr/bin/env python3
# ============================================================================
# scripts/lsp-mcp/servers/powershell_server.py -- PowerShellEditorServices
#                                                  (.ps1 / .psm1 / .psd1).
#
# Owner: TODO-07 PowerShellEditorServices Integration in 00-infrastructure.
#
# Spawns Microsoft PowerShellEditorServices (PSES) for the ~1.1k lines of
# PowerShell under scripts/. PSES is the painful one of the five LSPs in
# this bridge: it is NOT a standalone binary. It is a PowerShell module
# that runs inside `pwsh`, and starting it correctly requires:
#
#   1. pwsh 7.x on PATH (Windows PowerShell 5.1 is NOT supported by PSES).
#   2. The PowerShellEditorServices module discoverable on PSModulePath
#      OR pre-installed under a VS Code extension directory.
#   3. The right startup recipe -- specifically, invoking the module's
#      shipped Start-EditorServices.ps1 entry script via `pwsh -File`,
#      not via `pwsh -Command "Import-Module ...; Start-EditorServices"`.
#      The shipped script imports the sibling .psd1 itself and accepts
#      every parameter as an argv token. Going through `-Command`
#      requires escaping every space-bearing path through TWO levels of
#      PowerShell parsing; getting it wrong causes silent handshake
#      failures that pre-implementation Codex review pinned as the
#      most common LSP-bridge bug.
#
# Five design decisions worth flagging here so a future maintainer does
# not undo them in good faith:
#
#   1. -File invocation, not -Command. Per the upstream PSES README and
#      the source of Start-EditorServices.ps1, the supported start path
#      is `pwsh -File <module-root>/Start-EditorServices.ps1 -HostName
#      <name> -HostProfileId <id> ...`. Codex design review flagged
#      `-Command` as fragile; we adopt `-File` to keep argv tokens
#      Python-managed.
#
#   2. PSES discovery has TWO probes. The primary is
#      `Get-Module -ListAvailable PowerShellEditorServices` which
#      handles `Install-Module` installs. The fallback is a glob over
#      `~/.vscode*/extensions/ms-vscode.powershell-*/modules/
#      PowerShellEditorServices*/PowerShellEditorServices.psd1` because
#      the VS Code PowerShell extension ships PSES under its own
#      extension path WITHOUT registering it on PSModulePath. Microsoft
#      official docs show importing PSES from that location; treating
#      it as discoverable closes a real usability gap.
#
#   3. LogPath is a DIRECTORY (not a file). The upstream
#      StartEditorServicesCommand.cs treats LogPath as the folder where
#      it writes StartEditorServices-<pid>.log; passing a file path
#      causes PSES to refuse to start. Codex design review caught this.
#
#   4. Tempdir cleanup is failure-only. We create a tempdir per spawn
#      to host the LogPath + SessionDetailsPath. On a successful
#      handshake we LEAVE the tempdir in place because PSES continues
#      to write to LogPath for the lifetime of the session;
#      tearing it down out from under a running pwsh would corrupt
#      the log. On any spawn-time failure (handshake timeout, pwsh
#      crash, version mismatch caught after tempdir creation) we
#      clean it up. A future LspSubprocess shutdown-hook retrofit
#      could clean up successful sessions on bridge exit; that is a
#      bridge-skeleton change, not a per-language deliverable.
#
#   5. install_hint() is REASON-SENSITIVE. clangd/asm/bash/python
#      install_hint() returns one fixed string because there is one
#      possible failure (binary missing). PSES has THREE distinct
#      failures (pwsh missing, pwsh too old, PSES module missing) and
#      each one needs different remediation. The hint chooses the
#      right command based on what is_available() observed.
#
# Graceful-skip contract mirrors the four sibling modules: is_available()
# returns False when ANY of the three checks fails; spawn() raises
# LspError with kind 'lsp-binary-missing', which the bridge's SKIP path
# surfaces as exit-0 output. CI hosts without pwsh stay green, dev
# hosts without PSES stay green, dev hosts with Windows PowerShell 5.1
# (instead of pwsh 7.x) stay green.
# ============================================================================

from __future__ import annotations

import functools
import glob
import os
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Any, NamedTuple, Optional

from lsp_client import (LspError, LspSubprocess,
                        report_unconfirmed_shutdown)


PWSH_BIN = "pwsh"
LANG_TAG = "ps1"

_VERSION_PROBE_TIMEOUT_S = 5.0
_PSES_DISCOVERY_TIMEOUT_S = 8.0
_INITIALIZE_TIMEOUT_S = 30.0

_HOST_NAME = "Impossible OS LSP-MCP Bridge"
_HOST_PROFILE_ID = "io.impossible.lsp-mcp-bridge"
_HOST_VERSION = "1.0"
_LOG_LEVEL = "Normal"


class _Availability(NamedTuple):
    """Reason-sensitive availability state.

    `available` is True only when all three preconditions pass; the
    three Optional fields name what is_available() actually saw so
    install_hint() can choose the right remediation message without
    re-running the probes."""
    available: bool
    pwsh_path: Optional[str]
    pwsh_version: Optional[str]
    pses_psd1: Optional[str]
    failure_reason: Optional[str]


@functools.lru_cache(maxsize=1)
def _probe() -> _Availability:
    """Run all three preconditions: pwsh on PATH, pwsh >= 7.0, PSES
    module discoverable. Returns a structured result so install_hint()
    can speak the right language. Cached for the lifetime of the
    process; the bridge does not support hot-swapping pwsh installs."""
    pwsh_path = shutil.which(PWSH_BIN)
    if pwsh_path is None:
        return _Availability(False, None, None, None, "pwsh-missing")

    # `pwsh --version` prints "PowerShell 7.4.1" or similar. Parse the
    # major version; reject anything < 7.0 because PSES does not
    # support Windows PowerShell 5.1 and the TODO checklist explicitly
    # requires pwsh 7.x.
    try:
        result = subprocess.run(
            [pwsh_path, "--version"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=_VERSION_PROBE_TIMEOUT_S,
        )
    except (OSError, subprocess.TimeoutExpired):
        return _Availability(False, pwsh_path, None, None, "pwsh-broken")
    if result.returncode != 0:
        return _Availability(False, pwsh_path, None, None, "pwsh-broken")
    version_line = result.stdout.decode("utf-8", errors="replace").strip()
    major = _parse_pwsh_major(version_line)
    if major is None:
        return _Availability(False, pwsh_path, version_line, None, "pwsh-version-unparseable")
    if major < 7:
        return _Availability(False, pwsh_path, version_line, None, "pwsh-too-old")

    pses_psd1 = _discover_pses(pwsh_path)
    if pses_psd1 is None:
        return _Availability(False, pwsh_path, version_line, None, "pses-missing")

    # The .psd1 was found but the install MUST also ship the
    # Start-EditorServices.ps1 entry script in the same directory --
    # that's the canonical PSES startup path. A standalone .psd1
    # without the entry script is a corrupt or incomplete install
    # (e.g. someone copied just the manifest). Catch this in _probe()
    # so is_available() returns False and install_hint() can speak
    # to the right failure mode (Codex consistency review caught
    # spawn() raising with the wrong install_hint when the probe
    # had reported available=True).
    if _start_editor_services_script(pses_psd1) is None:
        return _Availability(False, pwsh_path, version_line, pses_psd1, "pses-entrypoint-missing")

    return _Availability(True, pwsh_path, version_line, pses_psd1, None)


def _parse_pwsh_major(version_line: str) -> Optional[int]:
    """Extract the major version digit from `pwsh --version` output.

    Output shape: "PowerShell 7.4.1" (sometimes additional
    annotations). Returns None if no recognizable digit appears."""
    parts = version_line.split()
    for token in parts:
        # "7.4.1" -- take the first dotted component that looks numeric.
        head = token.split(".")[0]
        if head.isdigit():
            return int(head)
    return None


def _discover_pses(pwsh_path: str) -> Optional[str]:
    """Locate PowerShellEditorServices.psd1. Two probes:

    Probe 1: ask pwsh itself via `Get-Module -ListAvailable`. This
    catches `Install-Module PowerShellEditorServices -Scope CurrentUser`
    and any PSModulePath registration. Returns the .psd1 path on
    stdout, empty on miss.

    Probe 2: glob common VS Code extension layouts. Microsoft's
    PowerShell VS Code extension ships PSES under its own extension
    directory WITHOUT registering it on PSModulePath, so probe 1
    misses it. Probe 2 tries `~/.vscode/extensions/...` and
    `~/.vscode-server/extensions/...` (Remote / WSL).

    Both probes are cheap; failure of one means the other might still
    succeed. Returns the first .psd1 that exists, or None."""
    # Probe 1: pwsh-driven discovery.
    try:
        result = subprocess.run(
            [
                pwsh_path,
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                "Get-Module -ListAvailable PowerShellEditorServices "
                "| Select-Object -First 1 -ExpandProperty Path",
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=_PSES_DISCOVERY_TIMEOUT_S,
        )
    except (OSError, subprocess.TimeoutExpired):
        result = None
    if result is not None and result.returncode == 0:
        candidate = result.stdout.decode("utf-8", errors="replace").strip()
        # Get-Module -ExpandProperty Path returns the .psd1 path
        # directly when found, or empty when not.
        if candidate and os.path.isfile(candidate):
            return candidate

    # Probe 2: glob VS Code extension layouts. We accept the first
    # match because multiple installations of the PowerShell extension
    # are unusual and any one of them ships a working PSES.
    home = os.path.expanduser("~")
    glob_patterns = [
        os.path.join(
            home, ".vscode", "extensions",
            "ms-vscode.powershell-*", "modules",
            "PowerShellEditorServices*", "PowerShellEditorServices.psd1",
        ),
        os.path.join(
            home, ".vscode-server", "extensions",
            "ms-vscode.powershell-*", "modules",
            "PowerShellEditorServices*", "PowerShellEditorServices.psd1",
        ),
    ]
    for pattern in glob_patterns:
        matches = sorted(glob.glob(pattern))
        for match in matches:
            if os.path.isfile(match):
                return match

    return None


def is_available() -> bool:
    """True iff pwsh 7.x is on PATH AND PowerShellEditorServices is
    discoverable. Three failures collapse to False; install_hint()
    speaks to which one was hit."""
    return _probe().available


def install_hint() -> str:
    """Reason-sensitive install guidance.

    Differs in shape from clangd/asm/bash/python install_hint() (each
    of which returns one fixed string) because PSES has three
    distinct failure modes and each one needs different remediation.
    The hint chooses based on what _probe() observed; sinks like
    SKIP-banner formatting and the JSON envelope read this string
    verbatim, so keep it under one line and grep-friendly."""
    state = _probe()
    if state.available:
        return "PowerShellEditorServices already installed"
    reason = state.failure_reason
    if reason == "pwsh-missing":
        return (
            "install pwsh 7.x: apt install powershell "
            "(or download from https://aka.ms/powershell-release)"
        )
    if reason in ("pwsh-broken", "pwsh-version-unparseable"):
        return (
            f"pwsh on PATH is not runnable (probe failed); reinstall pwsh 7.x "
            f"from https://aka.ms/powershell-release (saw: {state.pwsh_version!r})"
        )
    if reason == "pwsh-too-old":
        return (
            f"PSES requires pwsh 7.x; this host has {state.pwsh_version!r}. "
            "Install pwsh 7 from https://aka.ms/powershell-release"
        )
    if reason == "pses-missing":
        return (
            "pwsh 7.x is present but PowerShellEditorServices module is missing; "
            "pwsh -NoProfile -Command 'Install-Module PowerShellEditorServices "
            "-Scope CurrentUser -Force'"
        )
    if reason == "pses-entrypoint-missing":
        return (
            f"PowerShellEditorServices found at {state.pses_psd1} but the "
            "Start-EditorServices.ps1 entry script is missing -- this install "
            "is corrupt or incomplete; reinstall with pwsh -NoProfile -Command "
            "'Install-Module PowerShellEditorServices -Scope CurrentUser -Force'"
        )
    return "pwsh + PowerShellEditorServices not available (unknown reason)"


def _bundled_modules_path(psd1_path: str) -> str:
    """Compute the BundledModulesPath argument from a .psd1 location.

    PSES expects BundledModulesPath to point at a directory that
    CONTAINS a `PowerShellEditorServices` subfolder. We support two
    install layouts:

        VS Code / GitHub-zip:
          <bundle-root>/PowerShellEditorServices/PowerShellEditorServices.psd1

        Install-Module versioned (PowerShell Gallery):
          <bundle-root>/PowerShellEditorServices/<version>/PowerShellEditorServices.psd1

    In both, the bundle root is the parent of the directory named
    exactly `PowerShellEditorServices` in the ancestor chain. Walk
    upward looking for that ancestor; return its parent. If no such
    ancestor exists (unrecognized layout), fall back to the great-
    grandparent of psd1 -- that is the bundle root for the bare
    PowerShellEditorServices/PowerShellEditorServices.psd1 shape and
    a defensible best-guess for anything else."""
    psd1_resolved = Path(psd1_path).resolve()
    p = psd1_resolved.parent
    # Walk to filesystem root looking for the PSES module directory.
    # Path.parent stabilizes at root (`/` on POSIX, `C:\` on Windows);
    # the loop terminates when we either find the directory or hit
    # that fixed point.
    while True:
        if p.name == "PowerShellEditorServices":
            return str(p.parent)
        if p.parent == p:
            # Reached filesystem root without finding the module dir.
            # Fall back to the unversioned layout assumption: bundle
            # root is two levels up from the .psd1 file. This matches
            # the VS Code zip shape; for hand-rolled installs that
            # diverge from both supported layouts the resulting path
            # is wrong, but no worse than before this fix.
            return str(psd1_resolved.parent.parent)
        p = p.parent


def _start_editor_services_script(psd1_path: str) -> Optional[str]:
    """Locate the Start-EditorServices.ps1 entry script that ships
    inside the PSES module. PSES upstream documents this as the
    canonical startup path; using it lets us pass arguments as argv
    tokens rather than through `pwsh -Command` quoting."""
    psd1_dir = Path(psd1_path).resolve().parent
    candidate = psd1_dir / "Start-EditorServices.ps1"
    if candidate.is_file():
        return str(candidate)
    return None


def spawn(workspace_root: Path) -> LspSubprocess:
    """Spawn pwsh -File Start-EditorServices.ps1 -Stdio and complete
    the LSP initialize handshake.

    Returns a fully-initialized LspSubprocess with server_caps
    populated. Raises LspError('lsp-binary-missing') when any of the
    three preconditions fail (pwsh missing / pwsh too old / PSES
    missing); callers translate that into the JSON error envelope (or
    SKIP in the bridge self-test).

    Why -File over -Command: the upstream Start-EditorServices.ps1
    handles Import-Module + parameter binding correctly when invoked
    as a script; going through `-Command "Import-Module ...;
    Start-EditorServices ..."` requires escaping every path through
    two layers of PowerShell parsing. Codex design review pinned the
    -Command path as the source of "most common LSP-bridge failures"
    on PSES.

    Tempdir lifecycle: a per-spawn tempdir is created to host LogPath
    (a directory PSES writes log files into) and SessionDetailsPath
    (a file PSES writes the session's listening transport details
    into). On a successful handshake the tempdir is left in place
    because PSES continues writing to LogPath for the session
    lifetime. On any spawn-time failure (handshake timeout, pwsh
    crash) we clean it up here.
    """
    state = _probe()
    if not state.available:
        raise LspError(
            "lsp-binary-missing",
            f"pwsh + PowerShellEditorServices unavailable ({state.failure_reason})",
            lang=LANG_TAG,
            install_hint=install_hint(),
            failure_reason=state.failure_reason or "unknown",
        )

    pwsh_path = state.pwsh_path
    psd1_path = state.pses_psd1
    assert pwsh_path is not None and psd1_path is not None  # narrowed by available=True

    # Entrypoint presence was already verified by _probe() before it
    # set available=True. Asserting here makes the invariant explicit
    # for type-checkers and pins the contract: if _probe() ever stops
    # checking the entry script, this assert flags it before the
    # subprocess.Popen would crash with a less informative error.
    start_script = _start_editor_services_script(psd1_path)
    assert start_script is not None  # narrowed by _probe() invariant

    bundled_modules_path = _bundled_modules_path(psd1_path)

    # Per-spawn tempdir for LogPath + SessionDetailsPath. LogPath is a
    # DIRECTORY (PSES writes StartEditorServices-<pid>.log inside it,
    # confirmed by reading StartEditorServicesCommand.cs upstream).
    tempdir = tempfile.mkdtemp(prefix="lsp-mcp-pses-")
    log_dir = os.path.join(tempdir, "logs")
    session_details_path = os.path.join(tempdir, "session.json")
    try:
        os.makedirs(log_dir, exist_ok=True)

        cmd = [
            pwsh_path,
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-File", start_script,
            "-HostName", _HOST_NAME,
            "-HostProfileId", _HOST_PROFILE_ID,
            "-HostVersion", _HOST_VERSION,
            "-LogPath", log_dir,
            "-LogLevel", _LOG_LEVEL,
            "-BundledModulesPath", bundled_modules_path,
            "-SessionDetailsPath", session_details_path,
            "-Stdio",
        ]

        lsp = LspSubprocess(cmd, lang=LANG_TAG, cwd=workspace_root)

        # Honest capability surface. PSES advertises all five providers
        # below across every supported release; advertising them so the
        # tool-wiring handlers can rely on them being present.
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

        try:
            lsp.initialize(
                root_uri=workspace_root.as_uri(),
                capabilities=capabilities,
                initialization_options=None,
                timeout=_INITIALIZE_TIMEOUT_S,
            )
        except Exception:
            # Verdict REPORTED, not discarded: an unconfirmed death here
            # leaves PSES running with the force sweep as its only
            # remaining collector (section 26).
            # 2.0s, not the 1.0 default: PSES teardown is heavier than the
            # other servers' and that budget predates this change.
            report_unconfirmed_shutdown(
                lsp, "powershell spawner init failure", timeout=2.0)
            raise
    except BaseException:
        # Spawn-time failure: clean up tempdir BEFORE the LSP knows
        # about it (cleanup_paths walks at LspSubprocess.shutdown()
        # time, which never fires on the spawn-failure path because
        # lsp itself was never returned).
        shutil.rmtree(tempdir, ignore_errors=True)
        raise

    # Success path: register the tempdir on the LspSubprocess so the
    # bridge atexit / shutdown walk removes it AFTER the PSES
    # process has exited. Earlier revisions leaked the tempdir on
    # every successful session because no shutdown hook owned it;
    # the cleanup_paths attribute (added by TODO-07 in
    # 00-infrastructure file-change-lifecycle work) closes that gap
    # without spawn-side rmtree (which would race PSES still
    # writing logs).
    lsp.cleanup_paths.append(tempdir)
    return lsp


def required_capabilities() -> tuple[str, ...]:
    """Capabilities the PSES handshake MUST report back so the
    tool-wiring commit can depend on them. PSES today reliably
    advertises all five providers below across every supported
    release.

    publishDiagnostics is NOT in this tuple because it is a
    notification direction, not a server capability key (Codex
    design review correction). Diagnostic delivery is verified
    separately by polling lsp.diagnostics_by_uri when a section
    needs it."""
    return (
        "hoverProvider",
        "definitionProvider",
        "referencesProvider",
        "documentSymbolProvider",
        "workspaceSymbolProvider",
    )
