#!/usr/bin/env bash
# ============================================================================
# scripts/lsp-mcp/tests/test_bridge.sh -- regression pack for the LSP-MCP
#                                         bridge (TODO-07 in 00-infrastructure).
#
# Sub-test coverage:
#   1a -- --self-test exits 0 with zero LSPs spawned + the expected banner.
#   1b -- import smoke: both modules import; clangd spawner registered.
#   1c -- LspSubprocess lifecycle: spawn `cat`, shutdown, pgrep empty.
#   1d -- LSP JSON-RPC round-trip against a fake stdio LSP: initialize,
#         request/response demux under 5 concurrent calls, graceful
#         shutdown + post-shutdown request rejected.
#   1e -- LspError.to_envelope reserves `error` + `detail` against extras.
#   1f -- over-length header line -> protocol error + fast reject.
#   1g -- _find_repo_root resolves via __file__ (not CWD).
#   2a -- --self-test --lang=c: SKIP when clangd-19 missing, or OK with
#         a non-zero hover-byte count on src/kernel/main.c:kernel_main.
#   2b -- _hover_content_bytes handles MarkupContent / MarkedString /
#         list[MarkedString] response shapes uniformly.
#   2c -- clangd_server.is_available / install_hint / required_capabilities
#         surface is wired and stable.
#   3a -- --self-test --lang=asm: SKIP when asm-lsp missing, or OK with a
#         non-zero instruction-reference byte count on src/boot/entry.asm.
#   3b -- .asm-lsp.toml at repo root pins assembler = "nasm" (NASM flavor;
#         asm-lsp defaults to GAS).
#   3c -- asm_server surface (ASM_LSP_BIN / LANG_TAG / is_available /
#         required_capabilities / install_hint) stable.
#   4a -- --self-test --lang=sh: SKIP when bash-language-server missing,
#         or OK with the diagnostic-count banner on scripts/build.sh.
#   4b -- bash_server module imports + exposes the standard surface
#         (is_available / install_hint / spawn / required_capabilities).
#   4c -- bash_server.required_capabilities() returns a non-empty tuple
#         covering the providers the tool-wiring commit will route.
#   5a -- --self-test --lang=py: SKIP when pyright-langserver missing,
#         or OK with a non-zero workspace-symbol count on
#         scripts/todo-graph/build.py.
#   5b -- python_server module imports + exposes the standard surface
#         (PYRIGHT_BIN / LANG_TAG / is_available / install_hint / spawn /
#         required_capabilities).
#   5c -- python_server.required_capabilities() includes
#         workspaceSymbolProvider (the load-bearing cap for the smoke
#         path; pyright reliably advertises it across releases).
#   6a -- --self-test --lang=ps1: SKIP when pwsh+PSES unavailable, or
#         OK with a non-negative document-symbol count on
#         scripts/machines/run-qemu.ps1.
#   6b -- powershell_server module imports + exposes the standard
#         surface (PWSH_BIN / LANG_TAG / is_available / install_hint /
#         spawn / required_capabilities).
#   6c -- powershell_server.required_capabilities() includes the five
#         providers PSES advertises across all supported releases.
#   6d -- _caps_missing() honors the LSP "boolean | XxxOptions" cap
#         shape: True / `{}` / non-empty options dict are all
#         supported; False / None / key-absent are missing. Regression
#         guard for the 2026-04-25 PSES handshake bug where empty-
#         options dicts were treated as falsy and falsely reported
#         missing.
#   6e -- _bundled_modules_path() handles BOTH the unversioned VS Code
#         zip layout AND the versioned Install-Module layout. Regression
#         guard for the 2026-04-25 PSES bundle-root bug where versioned
#         installs returned the version dir instead of the bundle root.
#   6f -- _Availability discriminates pses-entrypoint-missing from
#         pses-missing, and install_hint() returns the right reinstall
#         text for each. Regression guard for the consistency-review
#         bug where a corrupt install (PSES.psd1 present but
#         Start-EditorServices.ps1 absent) returned "already installed"
#         as the install hint.
#   7a -- --self-test --tools introspects 6 MCP tools with the
#         required-params contract: hover/definition/references need
#         (path, line, character); diagnostics/document_symbol need
#         (path); workspace_symbol needs (query).
#   7b -- _dispatch_path() sandbox: hostile absolute path
#         (/etc/passwd) rejected with lsp-path-outside-workspace;
#         nonexistent path rejected with lsp-path-not-found;
#         unsupported extension (.md) rejected with lsp-path-
#         unsupported-extension; relative path resolves against
#         workspace_root (not CWD).
#   7c -- _EXT_TO_LANG maps every documented extension (.c/.h/.asm/
#         .S/.sh/.bash/.py/.ps1/.psm1/.psd1). _LANG_TO_LSP_LANGUAGE_ID
#         maps every language to the spec-correct LSP languageId
#         ("c"/"asm"/"shellscript"/"python"/"powershell").
#   7d -- Normalizers: _normalize_hover() handles MarkupContent /
#         MarkedString / list / None; _normalize_locations() picks
#         targetUri+targetRange for LocationLink and uri+range for
#         Location.
#   7e -- Read-only boundary gate: LspSubprocess.request() rejects
#         every method in _FORBIDDEN_LSP_METHODS with kind
#         lsp-method-forbidden BEFORE any wire I/O. Protects against
#         a future MCP tool handler or refactor reaching a write-
#         capable method.
#   7f -- Source-level boundary audit: no call to rename / applyEdit
#         / codeAction/execute / executeCommand exists anywhere
#         under the MCP tool handlers in bridge.py. Combined with
#         the runtime gate, this is defense in depth.
#   7g -- ensure_open() is idempotent and thread-safe: same URI
#         called N times from concurrent threads produces exactly
#         one didOpen wire message.
#   7h -- _validate_position() rejects negative, oversized, bool, and
#         non-integer LSP Position values. LSP spec says Position.line
#         and Position.character are uinteger (0 <= v < 2**31); the
#         early revision coerced int(True)==1 silently and forwarded
#         negative values to the server. Regression guard for Codex
#         adversarial review Medium finding.
#   8a -- --self-test --stress runs 100 concurrent hover calls through
#         clangd, asserts no Future leaks in lsp._pending. SKIPs when
#         clangd is not installed. Exercises _io_lock + _pending_lock
#         + _next_id_lock contention under 100-way fan-in. Demux
#         correctness is sub-test 8b's job (clangd hover responses do
#         not echo position, so swapped Futures cannot be detected
#         from the response payload alone).
#   8b -- Fake-LSP deterministic demux test: a stub stdio server that
#         delays + REORDERS responses (replies to req 5 first, then 1,
#         etc.) -- the bridge's _pending dict + Future demux MUST
#         deliver each reply to the correct caller. Catches demux bugs
#         that clangd stress can hide if the LSP happens to reply in
#         FIFO order.
#   8c -- LSP_MCP_TIMEOUT env-var override: spawn a stub server that
#         never replies, set LSP_MCP_TIMEOUT=0.05, call lsp.request()
#         with timeout=None, assert lsp-timeout envelope, assert
#         _pending dict is empty after, assert subprocess still alive
#         (timeouts must NOT kill the LSP). Also assert invalid env
#         values raise lsp-timeout-config-invalid.
#   9a -- LSP-process-leak detection: pgrep snapshots taken at harness
#         entry vs harness exit. Any NEW PID matching the 5 LSP binary
#         names (owned by this user) is a leak from a sub-test that
#         failed to shut down its LSP. Last sub-test in the file so
#         every prior sub-test has had a chance to clean up. Delta-
#         based to avoid false positives on dev hosts running an
#         editor's own clangd / pyright in parallel.
#
# Future commits append sub-tests for tool wiring, extended tool surface,
# file-change lifecycle, watchdog, structured logs, path sandboxing, and
# warm-start -- as each lands. The harness is the single entry point
# scripts/test-tooling.sh will wire into CI.
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

cd "$REPO_ROOT"

# LSP-process-leak detection (sub-test 9a): capture the set of LSP
# binary PIDs OWNED BY THIS USER at harness entry, diff against the
# set at harness exit. Any new PID matching one of the 5 supported
# LSP binaries is a leak from a sub-test that failed to clean up its
# subprocess. Codex pre-implementation review of the harness layer
# noted: comparing against ABSOLUTE counts gives false positives on
# dev hosts where the user's editor is also running clangd / pyright;
# delta-only is correct. Captured early so any sub-test that spawns
# its own LSP can be observed.
LSP_BIN_RE='clangd-19|asm-lsp|bash-language-server|pyright-langserver|pwsh'
LSP_PIDS_BEFORE=$(pgrep -u "$(id -u)" -f "$LSP_BIN_RE" 2>/dev/null | sort -n | tr '\n' ' ' || true)

pass=0
fail=0
run() {
    local name="$1"; shift
    if "$@"; then
        printf '[lsp-mcp-tests] PASS %s\n' "$name"
        pass=$((pass + 1))
    else
        printf '[lsp-mcp-tests] FAIL %s\n' "$name" >&2
        fail=$((fail + 1))
    fi
}

# --- 1a: --self-test exits 0 with expected banner ---------------------------
t_selftest() {
    local out
    out="$(python3 scripts/lsp-mcp/bridge.py --self-test 2>&1)"
    # Banner shapes:
    #   "[lsp-mcp] OK: 0 LSPs spawned, 6 tools registered, bridge ready"
    # OR "[lsp-mcp] SKIP: mcp SDK not installed; ..." (CI without SDK).
    echo "$out" | grep -qE '^\[lsp-mcp\] (OK: 0 LSPs spawned, 6 tools registered, bridge ready|SKIP: mcp SDK not installed)'
}

# --- 1b: import smoke --------------------------------------------------------
t_import() {
    python3 -c '
import sys
sys.path.insert(0, "scripts/lsp-mcp")
import bridge, lsp_client
assert callable(bridge.register_spawner)
assert bridge._LIVE_LSPS == {}
# After clangd + asm + bash + pyright + PSES wiring: the bridge
# auto-registers all five on import. The bridge proxies every
# language the repo uses; further additions are out-of-scope for
# the existing five-LSP charter.
assert "c" in bridge._LSP_SPAWNERS, bridge._LSP_SPAWNERS
assert "asm" in bridge._LSP_SPAWNERS, bridge._LSP_SPAWNERS
assert "sh" in bridge._LSP_SPAWNERS, bridge._LSP_SPAWNERS
assert "py" in bridge._LSP_SPAWNERS, bridge._LSP_SPAWNERS
assert "ps1" in bridge._LSP_SPAWNERS, bridge._LSP_SPAWNERS
assert hasattr(lsp_client, "LspSubprocess")
assert hasattr(lsp_client, "LspError")
'
}

# --- 1c: LspSubprocess lifecycle --------------------------------------------
t_subprocess_lifecycle() {
    python3 -c '
import sys, time
sys.path.insert(0, "scripts/lsp-mcp")
from lsp_client import LspSubprocess
c = LspSubprocess(["cat"], lang="test")
assert c.alive
pid = c.pid
c.shutdown(timeout=2.0)
time.sleep(0.2)
assert not c.alive, "cat still alive after shutdown"
'
    # Defensive: no stray `cat` processes remain after the Python exits.
    # Using -x for exact match so we dont catch unrelated long command lines.
    if pgrep -x cat >/dev/null; then
        return 1
    fi
    return 0
}

# --- 1d: JSON-RPC round-trip + concurrency + graceful shutdown --------------
t_jsonrpc_roundtrip() {
    python3 - << 'PY'
import sys, threading
sys.path.insert(0, "scripts/lsp-mcp")
from lsp_client import LspSubprocess, LspError

fake = r"""
import sys, json
def read_msg():
    hdr = b""
    while True:
        ch = sys.stdin.buffer.read(1)
        if not ch: return None
        hdr += ch
        if hdr.endswith(b"\r\n\r\n"): break
    for line in hdr.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            n = int(line.split(b":")[1].strip()); break
    return json.loads(sys.stdin.buffer.read(n).decode("utf-8"))
def write_msg(obj):
    b = json.dumps(obj).encode("utf-8")
    sys.stdout.buffer.write(b"Content-Length: " + str(len(b)).encode() + b"\r\n\r\n" + b)
    sys.stdout.buffer.flush()
while True:
    m = read_msg()
    if m is None: break
    if m.get("method") == "initialize":
        write_msg({"jsonrpc":"2.0","id":m["id"],"result":{"capabilities":{"hoverProvider":True}}})
    elif m.get("method") == "shutdown":
        write_msg({"jsonrpc":"2.0","id":m["id"],"result":None})
    elif m.get("method") == "exit":
        sys.exit(0)
    elif "id" in m:
        write_msg({"jsonrpc":"2.0","id":m["id"],"result":{"method":m["method"]}})
"""
lsp = LspSubprocess(["python3","-c",fake], lang="fake")
try:
    lsp.initialize("file:///tmp")
    assert lsp.server_caps.get("hoverProvider") is True, "initialize did not populate server_caps"
    results = {}
    def w(i): results[i] = lsp.request("m%d" % i, {"i": i})
    ts = [threading.Thread(target=w, args=(i,)) for i in range(5)]
    for t in ts: t.start()
    for t in ts: t.join()
    for i in range(5):
        assert results[i]["method"] == "m%d" % i, "response-id demux failure"
    lsp.shutdown(timeout=3.0)
    assert not lsp.alive, "graceful shutdown did not reap child"
    try:
        lsp.request("hover")
    except LspError as e:
        assert e.kind == "lsp-shutdown", "wrong error kind after shutdown"
    else:
        raise SystemExit("post-shutdown request did NOT raise")
finally:
    lsp.shutdown()
PY
}

# --- 1e: envelope reserves error/detail -------------------------------------
t_envelope_reserved() {
    python3 -c '
import sys
sys.path.insert(0, "scripts/lsp-mcp")
from lsp_client import LspError
e = LspError("kind-x", "detail-x", method="hover", lang="c")
env = e.to_envelope()
assert env["error"] == "kind-x"
assert env["detail"] == "detail-x"
assert env["method"] == "hover"
assert env["lang"] == "c"
# Extras that try to shadow reserved keys via direct dict poisoning.
e2 = LspError.__new__(LspError)
e2.kind = "real"; e2.detail = "real-d"
e2.extra = {"error": "POISON", "detail": "POISON", "keep": "yes"}
env2 = e2.to_envelope()
assert env2["error"] == "real", env2
assert env2["detail"] == "real-d", env2
assert env2["keep"] == "yes", env2
'
}

# --- 1f: over-length header line -> protocol error + fast reject --------
t_protocol_error_oversized_header() {
    python3 -c "
import sys, time
sys.path.insert(0, 'scripts/lsp-mcp')
from lsp_client import LspSubprocess, LspError
bad = r'''
import sys
# 10 KiB garbage with no CRLF anywhere -- forces malformed-header path.
sys.stdout.buffer.write(b\"X\" * 10240)
sys.stdout.buffer.flush()
while True:
    if not sys.stdin.buffer.read(1): break
'''
lsp = LspSubprocess(['python3','-c',bad], lang='bad')
for _ in range(40):
    if lsp._reader_dead: break
    time.sleep(0.05)
assert lsp._reader_dead, 'reader should have marked transport dead'
try:
    lsp.request('hover', timeout=1.0)
    raise SystemExit('expected fast-reject')
except LspError as e:
    assert e.kind == 'lsp-subprocess-exited', e.kind
lsp.shutdown()
"
}

# --- 1g: _find_repo_root resolves to this repo root ----------------------
t_workspace_root() {
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge
root = bridge._find_repo_root()
assert (root / 'todo').is_dir(), f'root missing todo/: {root}'
assert (root / 'scripts' / 'lsp-mcp').is_dir(), f'root missing scripts/lsp-mcp/: {root}'
"
}

# --- 2a: --self-test --lang=c ends in OK or SKIP, exit 0 ------------------
t_selftest_lang_c() {
    local out rc
    out="$(python3 scripts/lsp-mcp/bridge.py --self-test --lang=c 2>&1)"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        printf '[lsp-mcp-tests] debug (2a): exit=%s output: %s\n' "$rc" "$out" >&2
        return 1
    fi
    # Accept either the clangd OK banner with a non-zero byte count,
    # or the SKIP banner when clangd-19 is not installed on this host.
    echo "$out" | grep -qE "^\[lsp-mcp\] (OK: clangd spawned, hover on kernel_main returned [1-9][0-9]* bytes|SKIP: clangd not installed)"
}

# --- 2b: _hover_content_bytes handles all three LSP shapes ----------------
t_hover_content_bytes() {
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
from bridge import _hover_content_bytes
# MarkupContent
assert _hover_content_bytes({'contents': {'kind': 'markdown', 'value': 'hello'}}) == 5
# Single MarkedString (str)
assert _hover_content_bytes({'contents': 'hello world'}) == 11
# List of MarkedString / MarkupContent
mixed = [{'kind':'plaintext','value':'ab'}, 'cde', {'value':'fgh'}]
assert _hover_content_bytes({'contents': mixed}) == 2 + 3 + 3
# None + empty shapes
assert _hover_content_bytes(None) == 0
assert _hover_content_bytes({}) == 0
assert _hover_content_bytes({'contents': None}) == 0
assert _hover_content_bytes({'contents': 123}) == 0
"
}

# --- 2c: clangd_server surface API is stable ------------------------------
t_clangd_server_surface() {
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
from servers import clangd_server
assert clangd_server.CLANGD_BIN == 'clangd-19'
assert clangd_server.LANG_TAG == 'c'
assert isinstance(clangd_server.is_available(), bool)
assert callable(clangd_server.spawn)
# required_capabilities returns the five documented caps.
caps = clangd_server.required_capabilities()
for name in ('hoverProvider','definitionProvider','referencesProvider','documentSymbolProvider','workspaceSymbolProvider'):
    assert name in caps, name
# install_hint returns a non-empty string.
hint = clangd_server.install_hint()
assert isinstance(hint, str) and hint
"
}

# --- 3a: --self-test --lang=asm ends in OK or SKIP, exit 0 ----------------
t_selftest_lang_asm() {
    local out rc
    out="$(python3 scripts/lsp-mcp/bridge.py --self-test --lang=asm 2>&1)"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        printf '[lsp-mcp-tests] debug (3a): exit=%s output: %s\n' "$rc" "$out" >&2
        return 1
    fi
    # Accept either the asm-lsp OK banner with a non-zero byte count,
    # or the SKIP banner when asm-lsp is not installed on this host.
    echo "$out" | grep -qE "^\[lsp-mcp\] (OK: asm-lsp spawned, hover on mov returned instruction reference \([1-9][0-9]* bytes\)|SKIP: asm-lsp not installed)"
}

# --- 3b: .asm-lsp.toml pins NASM + schema-valid ISA + disables external
#         compiler diagnostics (NASM-only workspace) ----------------------
t_asm_lsp_config_nasm() {
    # File must exist at repo root, sit under [default_config], and
    # declare BOTH assembler = "nasm" AND instruction_set =
    # "x86/x86-64" (dual-mode covers the 32/64-bit transition in
    # src/boot/entry.asm; "x86-64" alone would parse 32-bit code
    # with wrong register widths).
    [ -f .asm-lsp.toml ] || return 1
    grep -qE '^\[default_config\]' .asm-lsp.toml || return 1
    grep -qE '^assembler\s*=\s*"nasm"' .asm-lsp.toml || return 1
    grep -qE '^instruction_set\s*=\s*"x86/x86-64"' .asm-lsp.toml || return 1
    # LSP-MCP integration adds [default_config.opts] disabling the
    # asm-lsp upstream gcc -> clang diagnostics fallback (meaningless
    # for a NASM-only workspace; floods the LSP with false errors).
    # Both fields required: diagnostics OFF + default-fallback OFF.
    grep -qE '^\[default_config\.opts\]' .asm-lsp.toml || return 1
    grep -qE '^diagnostics\s*=\s*false' .asm-lsp.toml || return 1
    grep -qE '^default_diagnostics\s*=\s*false' .asm-lsp.toml
}

# --- 3c: asm_server surface API is stable ---------------------------------
t_asm_server_surface() {
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
from servers import asm_server
assert asm_server.ASM_LSP_BIN == 'asm-lsp'
assert asm_server.LANG_TAG == 'asm'
assert isinstance(asm_server.is_available(), bool)
assert callable(asm_server.spawn)
caps = asm_server.required_capabilities()
for name in ('hoverProvider','definitionProvider','referencesProvider','documentSymbolProvider'):
    assert name in caps, name
hint = asm_server.install_hint()
assert isinstance(hint, str) and hint
"
}

# --- 4a: --self-test --lang=sh ends in OK or SKIP, exit 0 ----------------
t_selftest_lang_sh() {
    local out rc
    out="$(python3 scripts/lsp-mcp/bridge.py --self-test --lang=sh 2>&1)"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        printf '[lsp-mcp-tests] debug (4a): exit=%s output: %s\n' "$rc" "$out" >&2
        return 1
    fi
    # Accept either the bash-language-server OK banner (any non-negative
    # diagnostic count, including 0 -- "possibly empty if no issues" is
    # explicitly allowed by the TODO contract), or the SKIP banner when
    # bash-language-server is not installed on this host.
    echo "$out" | grep -qE "^\[lsp-mcp\] (OK: bash-language-server spawned, diagnostics on build\.sh returned [0-9]+ items|SKIP: bash-language-server not installed)"
}

# --- 4b: bash_server module imports + standard surface --------------------
t_bash_server_import() {
    # Surface check ONLY: do NOT call is_available() here. Sub-test 4a
    # already drives the real probe end-to-end; calling it again from
    # 4b would pay Node startup cost a second time on every CI run for
    # zero added coverage. Verify the symbol exists and is callable;
    # behavior is exercised by 4a.
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
from servers import bash_server
assert bash_server.BASH_LSP_BIN == 'bash-language-server'
assert bash_server.LANG_TAG == 'sh'
assert callable(bash_server.is_available)
assert callable(bash_server.install_hint)
assert callable(bash_server.spawn)
assert callable(bash_server.required_capabilities)
hint = bash_server.install_hint()
assert isinstance(hint, str) and 'bash-language-server' in hint
"
}

# --- 4c: bash_server.required_capabilities is stable ----------------------
t_bash_server_caps() {
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
from servers import bash_server
caps = bash_server.required_capabilities()
assert isinstance(caps, tuple) and caps, caps
# workspaceSymbolProvider is intentionally OMITTED -- bash-language-
# server versions are uneven on advertising it; the tool-wiring commit
# gates workspace_symbol routing on the runtime cap, not this list.
for name in ('hoverProvider','definitionProvider','referencesProvider','documentSymbolProvider'):
    assert name in caps, name
assert 'workspaceSymbolProvider' not in caps, 'see bash_server.required_capabilities() docstring'
"
}

# --- 5a: --self-test --lang=py ends in OK or SKIP, exit 0 ----------------
t_selftest_lang_py() {
    local out rc
    out="$(python3 scripts/lsp-mcp/bridge.py --self-test --lang=py 2>&1)"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        printf '[lsp-mcp-tests] debug (5a): exit=%s output: %s\n' "$rc" "$out" >&2
        return 1
    fi
    # Accept either the pyright OK banner with a non-zero result count
    # (workspace/symbol "main" against scripts/todo-graph/build.py
    # produces multiple matches once pyright finishes indexing), or the
    # SKIP banner when pyright-langserver is not installed on this host.
    echo "$out" | grep -qE "^\[lsp-mcp\] (OK: pyright spawned, workspace-symbol main returned [1-9][0-9]* results|SKIP: pyright not installed)"
}

# --- 5b: python_server module imports + standard surface ------------------
t_python_server_import() {
    # Surface check ONLY: do NOT call is_available() here. Sub-test 5a
    # already drives the real probe end-to-end; calling it again from
    # 5b would pay the Node startup cost a second time on every CI run
    # for zero added coverage. Verify the symbol exists and is callable;
    # behavior is exercised by 5a.
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
from servers import python_server
assert python_server.PYRIGHT_BIN == 'pyright-langserver'
assert python_server.PYRIGHT_CLI_BIN == 'pyright'
assert python_server.LANG_TAG == 'py'
assert callable(python_server.is_available)
assert callable(python_server.install_hint)
assert callable(python_server.spawn)
assert callable(python_server.required_capabilities)
hint = python_server.install_hint()
assert isinstance(hint, str) and 'pyright' in hint
"
}

# --- 5c: python_server.required_capabilities is stable --------------------
t_python_server_caps() {
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
from servers import python_server
caps = python_server.required_capabilities()
assert isinstance(caps, tuple) and caps, caps
# workspaceSymbolProvider IS asserted (unlike bash_server which drops
# it). Pyright ships the cap reliably; the sub-test 5a smoke path
# depends on it directly.
for name in ('hoverProvider','definitionProvider','referencesProvider','documentSymbolProvider','workspaceSymbolProvider'):
    assert name in caps, name
"
}

# --- 6a: --self-test --lang=ps1 ends in OK or SKIP, exit 0 ---------------
t_selftest_lang_ps1() {
    local out rc
    out="$(python3 scripts/lsp-mcp/bridge.py --self-test --lang=ps1 2>&1)"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        printf '[lsp-mcp-tests] debug (6a): exit=%s output: %s\n' "$rc" "$out" >&2
        return 1
    fi
    # Accept either the PSES OK banner with a non-negative document-
    # symbol count (an empty array is legitimate per LSP spec; large
    # .ps1 files like run-qemu.ps1 produce many), or the SKIP banner
    # when pwsh 7.x and/or PowerShellEditorServices is not installed.
    echo "$out" | grep -qE "^\[lsp-mcp\] (OK: PSES spawned, document-symbol returned [0-9]+ results|SKIP: PSES not installed)"
}

# --- 6b: powershell_server module imports + standard surface --------------
t_powershell_server_import() {
    # Surface check ONLY. 6a drives the real probe end-to-end; calling
    # is_available() here would re-pay the pwsh + PSES probe cost.
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
from servers import powershell_server
assert powershell_server.PWSH_BIN == 'pwsh'
assert powershell_server.LANG_TAG == 'ps1'
assert callable(powershell_server.is_available)
assert callable(powershell_server.install_hint)
assert callable(powershell_server.spawn)
assert callable(powershell_server.required_capabilities)
hint = powershell_server.install_hint()
assert isinstance(hint, str) and hint
"
}

# --- 6c: powershell_server.required_capabilities is stable ----------------
t_powershell_server_caps() {
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
from servers import powershell_server
caps = powershell_server.required_capabilities()
assert isinstance(caps, tuple) and caps, caps
for name in ('hoverProvider','definitionProvider','referencesProvider','documentSymbolProvider','workspaceSymbolProvider'):
    assert name in caps, name
# publishDiagnostics is a notification path, not a server cap key;
# must NOT be advertised as required (Codex design review correction).
assert 'publishDiagnostics' not in caps, 'publishDiagnostics is a notification, not a cap'
"
}

# --- 6e: _bundled_modules_path handles both PSES install layouts ---------
t_bundled_modules_path_layouts() {
    python3 -c "
import sys, tempfile
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
from servers.powershell_server import _bundled_modules_path

# Versioned Install-Module layout: <root>/PowerShellEditorServices/<ver>/PSES.psd1
with tempfile.TemporaryDirectory() as td:
    psd1 = Path(td) / 'PowerShellEditorServices' / '4.4.0' / 'PowerShellEditorServices.psd1'
    psd1.parent.mkdir(parents=True)
    psd1.touch()
    expected = str(Path(td).resolve())
    got = _bundled_modules_path(str(psd1))
    assert got == expected, f'versioned: got {got!r} expected {expected!r}'

# Unversioned VS Code / GitHub zip layout: <root>/PowerShellEditorServices/PSES.psd1
with tempfile.TemporaryDirectory() as td:
    psd1 = Path(td) / 'PowerShellEditorServices' / 'PowerShellEditorServices.psd1'
    psd1.parent.mkdir(parents=True)
    psd1.touch()
    expected = str(Path(td).resolve())
    got = _bundled_modules_path(str(psd1))
    assert got == expected, f'unversioned: got {got!r} expected {expected!r}'

# Nested VS Code extension layout: ~/.vscode/extensions/ms-vscode.powershell-X/modules/PowerShellEditorServices/PSES.psd1
with tempfile.TemporaryDirectory() as td:
    psd1 = Path(td) / 'modules' / 'PowerShellEditorServices' / 'PowerShellEditorServices.psd1'
    psd1.parent.mkdir(parents=True)
    psd1.touch()
    expected = str((Path(td) / 'modules').resolve())
    got = _bundled_modules_path(str(psd1))
    assert got == expected, f'vscode-ext: got {got!r} expected {expected!r}'
"
}

# --- 6f: install_hint reason discrimination ------------------------------
t_powershell_install_hint_reasons() {
    # Surface the install_hint() reason logic without spawning pwsh.
    # Replace ps._probe with a stub that returns a chosen _Availability
    # so install_hint() reads it. The lru_cache wrapper is shadowed by
    # the assignment; the stubbed function does not need cache_clear.
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
import servers.powershell_server as ps
from servers.powershell_server import _Availability, install_hint

def stub(av):
    ps._probe = lambda _av=av: _av  # type: ignore[assignment]

# pwsh-missing -- generic install pointer.
stub(_Availability(False, None, None, None, 'pwsh-missing'))
h = install_hint()
assert 'pwsh' in h.lower() and ('apt install' in h or 'aka.ms' in h), h

# pwsh-too-old -- must surface the observed version string.
stub(_Availability(False, '/usr/bin/pwsh', 'PowerShell 5.1.0', None, 'pwsh-too-old'))
h = install_hint()
assert 'pwsh 7' in h and '5.1.0' in h, h

# pses-missing -- Install-Module remediation, NOT 'corrupt' wording.
stub(_Availability(False, '/usr/bin/pwsh', 'PowerShell 7.4.1', None, 'pses-missing'))
h = install_hint()
assert 'Install-Module PowerShellEditorServices' in h, h
assert 'corrupt' not in h, h

# pses-entrypoint-missing -- distinct from pses-missing; must NOT say
# 'already installed'. Must mention Start-EditorServices.ps1 + reinstall.
stub(_Availability(False, '/usr/bin/pwsh', 'PowerShell 7.4.1',
                  '/some/path/PowerShellEditorServices/PowerShellEditorServices.psd1',
                  'pses-entrypoint-missing'))
h = install_hint()
assert 'Start-EditorServices.ps1' in h, h
assert 'corrupt' in h or 'incomplete' in h, h
assert 'already installed' not in h, h
"
}

# --- 6d: _caps_missing honors LSP boolean|options cap shape ---------------
t_caps_missing_options_shape() {
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
from bridge import _caps_missing
required = ('hoverProvider','definitionProvider')
# Supported variants per LSP 3.17:
#   - True (boolean)
#   - {} (default options)
#   - {'workDoneProgress': True} (non-empty options)
assert _caps_missing({'hoverProvider': True, 'definitionProvider': True}, required) == []
assert _caps_missing({'hoverProvider': {}, 'definitionProvider': {}}, required) == []
assert _caps_missing({'hoverProvider': {'workDoneProgress': True}, 'definitionProvider': {'linkSupport': False}}, required) == []
# Unsupported variants:
assert _caps_missing({'hoverProvider': False, 'definitionProvider': True}, required) == ['hoverProvider']
assert _caps_missing({'hoverProvider': None, 'definitionProvider': True}, required) == ['hoverProvider']
assert _caps_missing({'definitionProvider': True}, required) == ['hoverProvider']
assert _caps_missing({}, required) == ['hoverProvider', 'definitionProvider']
"
}

# --- 7a: --self-test --tools schema introspection ------------------------
t_selftest_tools() {
    local out rc
    out="$(python3 scripts/lsp-mcp/bridge.py --self-test --tools 2>&1)"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        printf '[lsp-mcp-tests] debug (7a): exit=%s output: %s\n' "$rc" "$out" >&2
        return 1
    fi
    # Accept either OK banner with all 6 tool names, or SKIP when the
    # mcp SDK is not installed on this host.
    if echo "$out" | grep -qE '^\[lsp-mcp\] SKIP: mcp SDK not installed'; then
        return 0
    fi
    echo "$out" | grep -qE '^\[lsp-mcp\] OK: 6 tools registered \(hover, definition, references, diagnostics, workspace_symbol, document_symbol\)$' || return 1
    # Each tool's schema must have the right required params.
    python3 -c "
import json, subprocess, sys
r = subprocess.run(['python3', 'scripts/lsp-mcp/bridge.py', '--self-test', '--tools'],
                   capture_output=True, text=True)
if r.returncode != 0 or '[lsp-mcp] SKIP' in r.stdout:
    sys.exit(0)
# Find the JSON block: everything up to the blank line before the banner.
end = r.stdout.find('[lsp-mcp] OK:')
assert end > 0, 'banner missing'
schemas = json.loads(r.stdout[:end])
expected = {
    'hover': (['path','line','character'], []),
    'definition': (['path','line','character'], []),
    'references': (['path','line','character'], ['include_declaration']),
    'diagnostics': (['path'], []),
    'document_symbol': (['path'], []),
    'workspace_symbol': (['query'], ['lang']),
}
for name, (req, opt) in expected.items():
    s = schemas[name]
    assert sorted(s['required']) == sorted(req), f'{name} required: {s[\"required\"]} != {req}'
    assert sorted(s['optional']) == sorted(opt), f'{name} optional: {s[\"optional\"]} != {opt}'
"
}

# --- 7b: _dispatch_path sandbox + routing ---------------------------------
t_dispatch_path_sandbox() {
    python3 -c "
import sys
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge
from lsp_client import LspError

ws = Path.cwd()

# Hostile absolute path outside workspace.
try:
    bridge._dispatch_path('/etc/passwd', ws)
    raise SystemExit('expected lsp-path-outside-workspace')
except LspError as e:
    assert e.kind == 'lsp-path-outside-workspace', e.kind

# Nonexistent path.
try:
    bridge._dispatch_path('src/nope-nonexistent-xyz.c', ws)
    raise SystemExit('expected lsp-path-not-found')
except LspError as e:
    assert e.kind == 'lsp-path-not-found', e.kind

# Unsupported extension.
try:
    bridge._dispatch_path('CLAUDE.md', ws)
    raise SystemExit('expected lsp-path-unsupported-extension')
except LspError as e:
    assert e.kind == 'lsp-path-unsupported-extension', e.kind

# Valid relative path resolves against workspace_root + reads content.
resolved, lang, text = bridge._dispatch_path('src/kernel/main.c', ws)
assert lang == 'c', lang
assert str(resolved).endswith('src/kernel/main.c'), resolved
assert 'kernel_main' in text, 'TOCTOU fix: text was not read at dispatch time'

# Valid ps1 path routes to ps1.
resolved, lang, text = bridge._dispatch_path('scripts/machines/run-qemu.ps1', ws)
assert lang == 'ps1', lang
assert text, 'empty ps1 text'
"
}

# --- 7c: _EXT_TO_LANG + _LANG_TO_LSP_LANGUAGE_ID coverage -----------------
t_dispatch_tables() {
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge

ext_map = bridge._EXT_TO_LANG
expected_ext = {'.c': 'c', '.h': 'c', '.asm': 'asm', '.S': 'asm',
                '.sh': 'sh', '.bash': 'sh', '.py': 'py',
                '.ps1': 'ps1', '.psm1': 'ps1', '.psd1': 'ps1'}
assert ext_map == expected_ext, ext_map

lang_id_map = bridge._LANG_TO_LSP_LANGUAGE_ID
expected_id = {'c': 'c', 'asm': 'asm', 'sh': 'shellscript',
               'py': 'python', 'ps1': 'powershell'}
assert lang_id_map == expected_id, lang_id_map
"
}

# --- 7d: Normalizers handle every LSP response shape ---------------------
t_normalizers() {
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
from bridge import _normalize_hover, _normalize_locations, _normalize_symbols

# Hover
assert _normalize_hover(None) == ''
assert _normalize_hover({}) == ''
assert _normalize_hover({'contents': None}) == ''
assert _normalize_hover({'contents': {'kind': 'markdown', 'value': 'hi'}}) == 'hi'
assert _normalize_hover({'contents': 'world'}) == 'world'
assert _normalize_hover({'contents': [{'value': 'a'}, 'b', {'kind':'plaintext','value':'c'}]}) == 'a\n\nb\n\nc'

# Locations -- Location / LocationLink / list / null
assert _normalize_locations(None) == []
assert _normalize_locations({'uri': 'u', 'range': 'r'}) == [{'uri': 'u', 'range': 'r'}]
assert _normalize_locations({'targetUri': 'tu', 'targetRange': 'tr'}) == [{'uri': 'tu', 'range': 'tr'}]
assert _normalize_locations([
    {'uri': 'u1', 'range': 'r1'},
    {'targetUri': 'tu2', 'targetRange': 'tr2', 'originSelectionRange': 'dropped'},
]) == [{'uri': 'u1', 'range': 'r1'}, {'uri': 'tu2', 'range': 'tr2'}]

# Symbols
assert _normalize_symbols(None) == []
assert _normalize_symbols([{'name': 'main'}]) == [{'name': 'main'}]
assert _normalize_symbols('not a list') == []
"
}

# --- 7e: Runtime boundary gate rejects forbidden methods ------------------
t_boundary_runtime_gate() {
    python3 -c "
import sys, subprocess
sys.path.insert(0, 'scripts/lsp-mcp')
from lsp_client import LspSubprocess, LspError, _FORBIDDEN_LSP_METHODS

# Grow-only contract: extending the deny set is fine; shrinking flags
# a policy change. Use issubset, not equality, so a 5th forbidden
# method does NOT false-fail this test while still catching removals.
# Mirrors the test_boundary.sh contract documented in its header.
required_min = {'textDocument/rename', 'textDocument/codeAction',
                'workspace/applyEdit', 'workspace/executeCommand'}
assert required_min.issubset(set(_FORBIDDEN_LSP_METHODS)), \
    f'required_min {required_min} not subset of {set(_FORBIDDEN_LSP_METHODS)}'

# Spawn a trivial fake LSP -- any binary that accepts stdin works.
# We do NOT send anything through LspSubprocess init; we just want to
# exercise request() method-name gating BEFORE any wire I/O.
lsp = LspSubprocess(['cat'], lang='test')
try:
    for method in sorted(_FORBIDDEN_LSP_METHODS):
        try:
            lsp.request(method, {}, timeout=0.5)
            raise SystemExit(f'{method} was not rejected')
        except LspError as e:
            assert e.kind == 'lsp-method-forbidden', f'{method}: got {e.kind}'
finally:
    lsp.shutdown(timeout=1.0)
"
}

# --- 7f: Source-level boundary audit -- delegated to test_boundary.sh -----
t_boundary_source_audit() {
    # The standalone test_boundary.sh imports _FORBIDDEN_LSP_METHODS
    # from lsp_client.py at runtime so the source-audit deny set
    # always tracks the runtime-gate deny set. Earlier inline grep
    # version hand-maintained a duplicate list; Codex consistency
    # review of the LSP-MCP bridge integration flagged that as drift
    # risk. Wrapping the standalone test keeps one source of truth.
    #
    # Capture stderr so a python3 import failure (vs an actual
    # boundary breach) gives the harness debug line concrete
    # context, rather than a generic FAIL. Codex Phase-2 review
    # caught the prior `>/dev/null 2>&1` swallowing.
    local out rc
    out="$(bash scripts/lsp-mcp/tests/test_boundary.sh 2>&1)"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        printf '[lsp-mcp-tests] debug (7f): exit=%s output: %s\n' "$rc" "$out" >&2
        return 1
    fi
    return 0
}

# --- 7h: _validate_position bounds + type gates ---------------------------
t_validate_position() {
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge
from lsp_client import LspError

# Reject negative
try:
    bridge._validate_position(-1, 0)
    raise SystemExit('negative line not rejected')
except LspError as e:
    assert e.kind == 'lsp-position-invalid', e.kind

# Reject oversized (>= 2**31)
try:
    bridge._validate_position(2**31, 0)
    raise SystemExit('oversized line not rejected')
except LspError as e:
    assert e.kind == 'lsp-position-invalid', e.kind

# Reject bool (int(True) == 1 silent coercion)
try:
    bridge._validate_position(True, 0)
    raise SystemExit('bool line not rejected')
except LspError as e:
    assert e.kind == 'lsp-position-invalid', e.kind

# Reject non-integer (strict int-only)
for bad in ('not_an_int', '10', 1.5, 1.9, 0.0, None):
    try:
        bridge._validate_position(bad, 0)
        raise SystemExit(f'{bad!r} not rejected (line)')
    except LspError as e:
        assert e.kind == 'lsp-position-invalid', e.kind
    try:
        bridge._validate_position(0, bad)
        raise SystemExit(f'{bad!r} not rejected (character)')
    except LspError as e:
        assert e.kind == 'lsp-position-invalid', e.kind

# Accept valid (and ONLY valid; no float-truncation)
assert bridge._validate_position(0, 0) == (0, 0)
assert bridge._validate_position(10, 5) == (10, 5)
assert bridge._validate_position(2**31 - 1, 2**31 - 1) == (2**31 - 1, 2**31 - 1)
"
}

# --- 7g: ensure_open() dedup under concurrency ----------------------------
t_ensure_open_dedup() {
    python3 -c "
import sys, threading
sys.path.insert(0, 'scripts/lsp-mcp')
from lsp_client import LspSubprocess

# Spawn a 'cat'-style passthrough. The reader sees only our outbound
# frames (cat echoes stdin to stdout, violating LSP framing -- that's
# OK for this test; we do not parse responses, we just count
# textDocument/didOpen notifications on the wire.
lsp = LspSubprocess(['cat'], lang='test')
try:
    # Swap in a spy notify() so we can count wire-format calls.
    sent = []
    lock = threading.Lock()
    orig_notify = lsp.notify
    def spy(method, params=None):
        with lock:
            sent.append((method, params))
    lsp.notify = spy  # type: ignore[assignment]

    # 16 concurrent threads all calling ensure_open() on the SAME URI.
    # Without the per-instance lock this would send 16 didOpen messages.
    barrier = threading.Barrier(16)
    def worker():
        barrier.wait()
        lsp.ensure_open('file:///tmp/x.py', 'python', 'print(1)', 1)
    threads = [threading.Thread(target=worker) for _ in range(16)]
    for t in threads: t.start()
    for t in threads: t.join()
    open_count = sum(1 for m, _ in sent if m == 'textDocument/didOpen')
    assert open_count == 1, f'expected 1 didOpen, got {open_count}'
    assert lsp.open_uris == {'file:///tmp/x.py'}, lsp.open_uris
finally:
    lsp.shutdown(timeout=1.0)
"
}

# --- 8a: --self-test --stress ------------------------------------------
t_selftest_stress() {
    local out rc
    out="$(python3 scripts/lsp-mcp/bridge.py --self-test --stress 2>&1)"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        printf '[lsp-mcp-tests] debug (8a): exit=%s output: %s\n' "$rc" "$out" >&2
        return 1
    fi
    # Accept the stress OK banner (no Future leaks; demux is 8b's job)
    # OR the SKIP banner when clangd is not installed.
    echo "$out" | grep -qE "^\[lsp-mcp\] (OK: stress [1-9][0-9]*/[1-9][0-9]* hover round-trips in [0-9.]+s.*no Future leaks|SKIP: clangd not installed)"
}

# --- 8b: fake-LSP deterministic out-of-order demux ---------------------
t_fake_lsp_reorder_demux() {
    python3 - << 'PY'
import sys, threading, time
sys.path.insert(0, 'scripts/lsp-mcp')
from lsp_client import LspSubprocess, LspError

# Stub LSP that REORDERS responses: it accumulates incoming requests,
# replies to id=N before id=1 (reverse order). If the bridge's demux
# is wrong, callers will see another caller's reply -- the test fails
# because returned method names won't match the requested method.
fake = r"""
import sys, json, threading, time

pending = []
lock = threading.Lock()

def read_msg():
    hdr = b''
    while True:
        ch = sys.stdin.buffer.read(1)
        if not ch: return None
        hdr += ch
        if hdr.endswith(b'\r\n\r\n'): break
    n = 0
    for line in hdr.split(b'\r\n'):
        if line.lower().startswith(b'content-length:'):
            n = int(line.split(b':')[1].strip()); break
    return json.loads(sys.stdin.buffer.read(n).decode('utf-8'))

def write_msg(obj):
    b = json.dumps(obj).encode('utf-8')
    sys.stdout.buffer.write(b'Content-Length: ' + str(len(b)).encode() + b'\r\n\r\n' + b)
    sys.stdout.buffer.flush()

EXPECT_N = 10
DEADLINE = 4.0

def flush_in_reverse():
    # Wait until EXPECT_N requests have queued OR DEADLINE expires;
    # then flush whatever we have in REVERSE order. Polling avoids
    # the race where one short sleep flushes an incomplete batch
    # and late callers hang. Codex post-implementation review of
    # this section flagged the single-sleep-then-flush pattern as
    # Low (test flakiness on slow schedulers).
    start = time.monotonic()
    while time.monotonic() - start < DEADLINE:
        with lock:
            if len(pending) >= EXPECT_N:
                break
        time.sleep(0.01)
    with lock:
        batch = pending[:]
        pending.clear()
    for m in reversed(batch):
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'echoed_method':m['method'],'echoed_id':m['id']}})

flusher_started = False
while True:
    m = read_msg()
    if m is None: break
    if m.get('method') == 'initialize':
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'capabilities':{}}})
    elif m.get('method') == 'shutdown':
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    elif m.get('method') == 'exit':
        break
    elif 'id' in m:
        with lock:
            pending.append(m)
        if not flusher_started:
            flusher_started = True
            threading.Thread(target=flush_in_reverse, daemon=True).start()
"""
lsp = LspSubprocess(['python3', '-c', fake], lang='fake')
try:
    lsp.initialize('file:///tmp/test')
    # Fire 10 concurrent requests with distinct method names; the
    # fake server replies in reverse. Each Future MUST resolve to the
    # response carrying its OWN method name -- not someone else's.
    results = {}
    errors = []
    def caller(i):
        try:
            r = lsp.request(f'm{i}', {'i': i}, timeout=5.0)
            results[i] = r
        except Exception as e:
            errors.append((i, str(e)))
    threads = [threading.Thread(target=caller, args=(i,)) for i in range(10)]
    for t in threads: t.start()
    for t in threads: t.join()
    assert not errors, f'errors: {errors}'
    for i, r in results.items():
        assert r['echoed_method'] == f'm{i}', \
            f'demux scrambled: req m{i} got back method {r["echoed_method"]!r}'
    assert len(results) == 10, f'expected 10 results, got {len(results)}'
finally:
    lsp.shutdown(timeout=2.0)
PY
}

# --- 8c: LSP_MCP_TIMEOUT env override ---------------------------------
t_timeout_env_override() {
    python3 - << 'PY'
import sys, os, time
sys.path.insert(0, 'scripts/lsp-mcp')
from lsp_client import LspSubprocess, LspError, _resolve_request_timeout, _DEFAULT_REQUEST_TIMEOUT_S

# Pure resolver tests (no subprocess).
assert _resolve_request_timeout(2.5) == 2.5
old = os.environ.pop('LSP_MCP_TIMEOUT', None)
try:
    assert _resolve_request_timeout(None) == _DEFAULT_REQUEST_TIMEOUT_S
    os.environ['LSP_MCP_TIMEOUT'] = '7.5'
    assert _resolve_request_timeout(None) == 7.5
    for bad in ('not-a-number', '-1', '0', 'inf', 'nan'):
        os.environ['LSP_MCP_TIMEOUT'] = bad
        try:
            _resolve_request_timeout(None)
            raise SystemExit(f'invalid env value {bad!r} not rejected')
        except LspError as e:
            assert e.kind == 'lsp-timeout-config-invalid', e.kind
finally:
    if old is None:
        os.environ.pop('LSP_MCP_TIMEOUT', None)
    else:
        os.environ['LSP_MCP_TIMEOUT'] = old

# Explicit-value validation: timeout=0 / -1 / inf / nan / bool / str
# all rejected at request() entry BEFORE Future registration.
import math as _math
for bad in (0, -1, 0.0, -1.5, _math.inf, _math.nan, True, False, 'oops', None):
    if bad is None:
        continue  # None is the env-fallback path, exercised above
    try:
        _resolve_request_timeout(bad)
        raise SystemExit(f'invalid explicit value {bad!r} not rejected')
    except LspError as e:
        assert e.kind == 'lsp-timeout-config-invalid', \
            f'{bad!r}: got kind={e.kind!r}'

# End-to-end: stub server that NEVER replies; env timeout forces
# request() to bail. After timeout: subprocess still alive, _pending
# emptied (no Future leak).
fake = r"""
import sys
def read_msg():
    hdr = b''
    while True:
        ch = sys.stdin.buffer.read(1)
        if not ch: return None
        hdr += ch
        if hdr.endswith(b'\r\n\r\n'): break
    n = 0
    for line in hdr.split(b'\r\n'):
        if line.lower().startswith(b'content-length:'):
            n = int(line.split(b':')[1].strip()); break
    import json
    return json.loads(sys.stdin.buffer.read(n).decode('utf-8'))
def write_msg(obj):
    import json
    b = json.dumps(obj).encode('utf-8')
    sys.stdout.buffer.write(b'Content-Length: ' + str(len(b)).encode() + b'\r\n\r\n' + b)
    sys.stdout.buffer.flush()
while True:
    m = read_msg()
    if m is None: break
    if m.get('method') == 'initialize':
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'capabilities':{}}})
    elif m.get('method') == 'shutdown':
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    elif m.get('method') == 'exit':
        break
    # else: silently swallow (never reply)
"""
os.environ['LSP_MCP_TIMEOUT'] = '0.05'
try:
    lsp = LspSubprocess(['python3', '-c', fake], lang='fake')
    try:
        lsp.initialize('file:///tmp/test')
        try:
            lsp.request('hover', {}, timeout=None)
            raise SystemExit('expected lsp-timeout')
        except LspError as e:
            assert e.kind == 'lsp-timeout', e.kind
        # _pending must be empty (Future was evicted on timeout).
        with lsp._pending_lock:
            assert len(lsp._pending) == 0, lsp._pending
        # Subprocess must still be alive (timeout MUST NOT kill the LSP).
        assert lsp.alive, 'subprocess killed by timeout (forbidden)'
    finally:
        lsp.shutdown(timeout=2.0)
finally:
    os.environ.pop('LSP_MCP_TIMEOUT', None)
PY
}

# --- 9a: LSP-process-leak detection (run LAST) ----------------------------
t_no_leaked_lsp_processes() {
    # Capture the post-harness PID set of LSP binaries owned by this
    # user. Any PID present here but NOT in LSP_PIDS_BEFORE was
    # spawned during the harness AND not cleaned up. A clean run
    # leaves the delta empty.
    #
    # Some sub-tests (8a/8b/8c) spawn fake stdio LSPs via `python3 -c
    # ... cat`; those don't match the real-LSP binary regex so they
    # don't show up here. The regex deliberately matches only the 5
    # production LSP binary names from the install table.
    local after delta
    after=$(pgrep -u "$(id -u)" -f "$LSP_BIN_RE" 2>/dev/null | sort -n | tr '\n' ' ' || true)
    delta=""
    for pid in $after; do
        case " $LSP_PIDS_BEFORE " in
            *" $pid "*) ;;
            *) delta="$delta $pid" ;;
        esac
    done
    if [ -n "$(echo "$delta" | tr -d ' ')" ]; then
        printf '[lsp-mcp-tests] debug (9a): leaked LSP PIDs:%s\n' "$delta" >&2
        # Show which binaries leaked so the error names them.
        for pid in $delta; do
            ps -o pid,comm,args -p "$pid" 2>/dev/null | tail -n +2 | sed 's/^/  /' >&2
        done
        return 1
    fi
    return 0
}

run "1a --self-test banner"              t_selftest
run "1b module import smoke"             t_import
run "1c LspSubprocess lifecycle + pgrep" t_subprocess_lifecycle
run "1d JSON-RPC round-trip + shutdown"  t_jsonrpc_roundtrip
run "1e envelope reserves keys"          t_envelope_reserved
run "1f over-length header rejected"     t_protocol_error_oversized_header
run "1g workspace root via __file__"     t_workspace_root
run "2a clangd self-test lang=c"         t_selftest_lang_c
run "2b hover_content_bytes shapes"      t_hover_content_bytes
run "2c clangd_server surface"           t_clangd_server_surface
run "3a asm self-test lang=asm"          t_selftest_lang_asm
run "3b .asm-lsp.toml pins NASM"         t_asm_lsp_config_nasm
run "3c asm_server surface"              t_asm_server_surface
run "4a bash self-test lang=sh"          t_selftest_lang_sh
run "4b bash_server import + surface"    t_bash_server_import
run "4c bash_server required caps"       t_bash_server_caps
run "5a pyright self-test lang=py"       t_selftest_lang_py
run "5b python_server import + surface"  t_python_server_import
run "5c python_server required caps"     t_python_server_caps
run "6a PSES self-test lang=ps1"         t_selftest_lang_ps1
run "6b powershell_server surface"       t_powershell_server_import
run "6c powershell_server required caps" t_powershell_server_caps
run "6d _caps_missing options shape"     t_caps_missing_options_shape
run "6e bundled_modules_path layouts"    t_bundled_modules_path_layouts
run "6f install_hint reason discrim"     t_powershell_install_hint_reasons
run "7a --self-test --tools schemas"     t_selftest_tools
run "7b dispatch_path sandbox"           t_dispatch_path_sandbox
run "7c ext + lang-id tables"            t_dispatch_tables
run "7d normalizers"                     t_normalizers
run "7e boundary runtime gate"           t_boundary_runtime_gate
run "7f boundary source audit"           t_boundary_source_audit
run "7g ensure_open dedup"               t_ensure_open_dedup
run "7h validate_position bounds"        t_validate_position
run "8a stress 100 concurrent hovers"    t_selftest_stress
run "8b fake-LSP reorder demux"          t_fake_lsp_reorder_demux
run "8c LSP_MCP_TIMEOUT env override"    t_timeout_env_override
# 9a runs LAST so every prior sub-test has had a chance to clean up.
run "9a no leaked LSP processes"         t_no_leaked_lsp_processes

printf '[lsp-mcp-tests] %d/%d sub-tests PASS\n' "$pass" "$((pass + fail))"
if [ "$fail" -gt 0 ]; then
    exit 1
fi
exit 0
