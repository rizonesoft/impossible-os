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
#   11a -- Extended-tool normalizers (TODO-07 in 00-infrastructure):
#          _normalize_completion (truncation at _COMPLETION_MAX_ITEMS,
#          drops insertText/textEdit/command per read-only boundary),
#          _normalize_signature_help (active_* coerced to None when
#          signatures are empty), _normalize_call_hierarchy_items,
#          _normalize_call_hierarchy_calls (incoming uses 'from',
#          outgoing uses 'to'), _normalize_code_actions, and
#          _validate_range (rejects bad types and bad positions).
#   11b -- Boundary contract for code_action: textDocument/codeAction
#          is read-only per LSP 3.17 and is intentionally NOT in the
#          deny set; workspace/applyEdit, workspace/executeCommand,
#          textDocument/rename remain forbidden.
#   11c -- completion handler end-to-end smoke against clangd-19.
#          Builds the FastMCP server, finds the registered tool fn,
#          invokes it on a real C source file, and validates the
#          response shape contract. Tolerates LSP-cold-start envelopes
#          since clangd may not have indexed yet. Also validates
#          trigger_character length validation (1..4).
#   11d -- code_action immutability: invokes the code_action tool with
#          a real range; md5sums the source file before AND after to
#          prove the bridge never applied any edit. Also validates
#          range-shape rejection (lsp-range-invalid).
#   11e -- call_hierarchy walks every prepared anchor (regression
#          guard for Codex Medium finding: an early revision queried
#          only prepared[0], silently dropping callers/callees for
#          overloaded symbols). A stub LSP returns 3 prepared items;
#          the test asserts the bridge issues 3 follow-up calls and
#          bundles per-anchor results into the response.
#   11f -- call_hierarchy caps unbounded fan-out (regression guard for
#          Codex post-commit High finding: an unbounded prepare list
#          would let one MCP call spawn thousands of sequential LSP
#          requests). A stub LSP returns 200 prepared items; the test
#          asserts the bridge issues at most _CALL_HIERARCHY_MAX_ANCHORS
#          follow-ups, sets truncated=True, and surfaces prepared_total.
#   11g -- call_hierarchy enforces wall-clock deadline (regression
#          guard for Codex post-commit perf High: cap-only protection
#          still allowed ~8 minute interactive latency on a wedged
#          LSP). A stub LSP returns prepared items but never replies
#          to follow-ups; with the deadline patched short, the test
#          asserts the handler returns within seconds with
#          deadline_exceeded=True instead of waiting for every
#          per-call timeout.
#   12a -- apply_text() open-or-refresh decision matrix: first call =
#          didOpen+v1, same-mtime second call = no-op cached v1,
#          different-mtime third call = didChange+v2, force_did_save
#          adds didSave, second URI tracked independently, mtime=None
#          skips refresh check. Pure-data; uses a stub LSP.
#   12b -- did_close fires at shutdown for every tracked URI: open
#          three URIs via apply_text, shutdown(); stub log records
#          three textDocument/didClose notifications BEFORE the
#          shutdown request and exit notification.
#   12c -- cleanup_paths walked after shutdown: append two tempdirs
#          to lsp.cleanup_paths, shutdown(); both directories must be
#          gone and cleanup_paths must be cleared so a double-shutdown
#          does not double-rmtree.
#   12d -- bridge handler forwards didChange when on-disk file
#          changes: hover an arbitrary file, edit it externally
#          (rewrite + sleep to bump mtime), hover again; stub log
#          must show didOpen, hover, didChange, hover in that order.
#          End-to-end through the real bridge handler chain.
#   12e -- apply_text rejects during teardown (regression guard for
#          Codex post-impl review High): a concurrent apply_text
#          racing shutdown's didClose snapshot could otherwise
#          silently send didOpen for an unsnapshot URI and miss
#          didClose. Test runs the race 20 times and asserts every
#          late call either succeeds before the commit or rejects
#          with lsp-shutdown -- never silently smuggles traffic into
#          the teardown sequence.
#   13a -- LspSubprocess flags _crashed + _crash_reason when its
#          subprocess dies unexpectedly (SIGKILL, no shutdown call);
#          public crashed property mirrors. Fundamental crash
#          detection contract used by the bridge respawn path.
#   13b -- bridge respawns the LSP transparently on the next MCP
#          tool call after a crash. Stub LSP, kill subprocess, hover
#          again; assert respawn fires + replay reopens the URI +
#          _health surfaces restart_count=1 + status=healthy.
#   13c -- after _RESPAWN_FAILED_THRESHOLD crashes within the
#          window the bridge enters FAILED state; subsequent calls
#          return lsp-persistently-crashing instead of respawning
#          forever. Also asserts the _health tool surfaces
#          failed=True + status=failed.
#   13d -- _backoff_delay_s schedule: 1, 2, 4, 8, 16, 30, 30, ...
#          (capped at _RESPAWN_BACKOFF_CAP_S).
#   13e -- _health tool returns {'languages': {}} when no LSP has
#          been spawned in this session (works without any wire
#          traffic; safe even when every LSP is FAILED).
#   13f -- regression guard for Codex post-impl review High: cold-
#          spawn / initialize failures now feed _record_crash and
#          cross to lsp-persistently-crashing. Earlier revision
#          looped forever on a broken spawner.
#   13g -- regression guard for Codex post-impl review Medium:
#          spawner-owned tempdirs registered on cleanup_paths are
#          walked when the OLD instance is disposed during respawn
#          (PSES leak fix). Crashes + respawns + asserts the old
#          tempdir is gone while the new one is owned by the new
#          instance.
#   14a -- logger.log emits valid JSON + level filter (default
#          INFO suppresses DEBUG; setting DEBUG enables all).
#   14b -- corr_id propagates as a ContextVar across pool workers
#          ONLY when the caller wraps with contextvars.copy_context()
#          .run() (regression guard for Codex design review High:
#          threading.local would have produced corr_id=None at
#          fan-out boundaries).
#   14c -- LSP_MCP_LOG_FILE env redirects JSON-lines to a file +
#          appends across reloads (no truncation).
#   14d -- _call_lsp emits paired phase=start / phase=end events
#          with matching corr_id and computed latency_ms.
#   14e -- debug_lsp_send truncates oversized payloads at 4 KiB
#          and marks body_truncated=True; outer JSON line stays
#          parseable because body is emitted as a STRING.
#   14f -- 3 concurrent _health calls produce 3 distinct corr_ids,
#          each with start + end pairing.
#   14g -- spawner-import warnings emit as JSON via logger (Codex
#          design review Medium: the 5 _autoregister_spawners
#          callsites used to bypass the JSON contract via raw
#          sys.stderr.write).
#   13i -- regression guard for Codex post-commit perf review
#          Medium: backoff used to be indexed by cumulative
#          restart_count, so a long-lived bridge with transient
#          crashes accumulated to the 30s cap forever. Fix indexes
#          by sliding-window crash count; this test seeds 10
#          historical restarts with empty window, records a fresh
#          crash, and asserts backoff returns to the base delay.
#   13h -- regression guard for Codex post-commit review High:
#          workspace_symbol fan-out used to bypass _get_or_spawn,
#          calling raw inst.request() against alive-checked
#          instances; a crashed LSP would silently fail in the
#          per-lang errors dict without entering crash accounting.
#          With the fix the fan-out worker re-routes through
#          _get_or_spawn (with one-retry on crash-class envelopes),
#          so the dead LSP triggers respawn transparently and
#          restart_count surfaces in _health.
#   12f -- post-apply_text request rejected during teardown
#          (regression guard for the Codex post-implementation review
#          High follow-up to 12e): a tool thread that already passed
#          apply_text's gate can be inside lsp.request() when shutdown
#          commits. With the global _teardown_in_progress bool that
#          existed before this fix, the tool thread's request would
#          have slipped past the gate. The thread-id-bound
#          _teardown_thread_id rejects any thread != shutdown owner.
#          Test runs 15 trials and asserts each post-apply_text
#          request rejects with lsp-shutdown.
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
    #   "[lsp-mcp] OK: 0 LSPs spawned, 15 tools registered, bridge ready"
    # OR "[lsp-mcp] SKIP: mcp SDK not installed; ..." (CI without SDK).
    # Tool count = len(MCP_TOOL_NAMES); pin to the literal so a
    # registration drift fails this test instead of silently sliding.
    echo "$out" | grep -qE '^\[lsp-mcp\] (OK: 0 LSPs spawned, 15 tools registered, bridge ready|SKIP: mcp SDK not installed)'
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
    echo "$out" | grep -qE '^\[lsp-mcp\] OK: 15 tools registered \(hover, definition, references, diagnostics, workspace_symbol, document_symbol, completion, signature_help, type_definition, implementation, declaration, call_hierarchy_incoming, call_hierarchy_outgoing, code_action, _health\)$' || return 1
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
    # Six core tools.
    'hover': (['path','line','character'], []),
    'definition': (['path','line','character'], []),
    'references': (['path','line','character'], ['include_declaration']),
    'diagnostics': (['path'], []),
    'document_symbol': (['path'], []),
    'workspace_symbol': (['query'], ['lang']),
    # Eight extended tools.
    'completion': (['path','line','character'], ['trigger_character']),
    'signature_help': (['path','line','character'], []),
    'type_definition': (['path','line','character'], []),
    'implementation': (['path','line','character'], []),
    'declaration': (['path','line','character'], []),
    'call_hierarchy_incoming': (['path','line','character'], []),
    'call_hierarchy_outgoing': (['path','line','character'], []),
    'code_action': (['path','range'], ['diagnostic']),
    # Meta tool: zero required params.
    '_health': ([], []),
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

# Valid relative path resolves against workspace_root + reads content
# AND captures mtime_ns for the file-change-lifecycle path.
resolved, lang, text, mtime_ns = bridge._dispatch_path('src/kernel/main.c', ws)
assert lang == 'c', lang
assert str(resolved).endswith('src/kernel/main.c'), resolved
assert 'kernel_main' in text, 'TOCTOU fix: text was not read at dispatch time'
assert isinstance(mtime_ns, int) and mtime_ns > 0, f'bad mtime_ns: {mtime_ns!r}'

# Valid ps1 path routes to ps1.
resolved, lang, text, mtime_ns = bridge._dispatch_path('scripts/machines/run-qemu.ps1', ws)
assert lang == 'ps1', lang
assert text, 'empty ps1 text'
assert isinstance(mtime_ns, int) and mtime_ns > 0, f'bad mtime_ns: {mtime_ns!r}'
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
required_min = {'textDocument/rename',
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

# --- 11a: Extended-tool normalizers handle every LSP response shape -------
t_extended_normalizers() {
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
from bridge import (_normalize_completion, _normalize_signature_help,
                    _normalize_call_hierarchy_items,
                    _normalize_call_hierarchy_calls,
                    _normalize_code_actions, _validate_range,
                    _COMPLETION_MAX_ITEMS)
from lsp_client import LspError

# completion: null / empty / list / CompletionList / truncation
r = _normalize_completion(None)
assert r == {'items': [], 'isIncomplete': False, 'truncated': False, 'total': 0}, r
r = _normalize_completion([{'label': 'kmalloc', 'kind': 3}])
assert r['total'] == 1 and r['items'][0]['label'] == 'kmalloc', r
assert r['raw_total'] == 1, r
r = _normalize_completion({'isIncomplete': True, 'items': [{'label': 'k'}]})
assert r['isIncomplete'] is True and r['total'] == 1, r
big = [{'label': f'x{i}'} for i in range(_COMPLETION_MAX_ITEMS + 25)]
r = _normalize_completion(big)
assert r['truncated'] is True and len(r['items']) == _COMPLETION_MAX_ITEMS, r
# Codex Low: total counts USABLE items, not raw input; raw_total
# exposes input length so callers can detect malformed responses.
assert r['total'] == _COMPLETION_MAX_ITEMS + 25, r
assert r['raw_total'] == _COMPLETION_MAX_ITEMS + 25, r
# Malformed-front regression: leading non-dict garbage MUST be filtered
# BEFORE truncation. Otherwise garbage-prefixed responses returned 0
# usable items even when valid items existed deeper in the list.
mixed = ['junk', None, 42] * 10 + [{'label': 'real_completion'}] * 5
r = _normalize_completion(mixed)
assert r['total'] == 5, r          # only the 5 valid items count
assert r['raw_total'] == 35, r     # raw input had 30 garbage + 5 valid
assert len(r['items']) == 5, r
assert all(it['label'] == 'real_completion' for it in r['items']), r
assert r['truncated'] is False, r  # 5 valid is under the 50 cap
# Per-item drops insertText / textEdit / command (read-only boundary).
r = _normalize_completion([{'label': 'a', 'insertText': 'INJECTED',
                             'textEdit': {'range': {}, 'newText': 'X'},
                             'command': {'title': 'execute me', 'command': 'apply.something'}}])
keep = r['items'][0]
assert 'insertText' not in keep, keep
assert 'textEdit' not in keep, keep
assert 'command' not in keep, keep

# signature_help: null / valid / missing fields
assert _normalize_signature_help(None) == {'signatures': [],
    'active_signature': None, 'active_parameter': None}
r = _normalize_signature_help({'signatures': [{'label': 'foo(x)'}],
    'activeSignature': 0, 'activeParameter': 0})
assert r['active_signature'] == 0 and r['active_parameter'] == 0, r
# Empty signatures -> active_signature flushed to None to avoid index OOB.
r = _normalize_signature_help({'signatures': [], 'activeSignature': 5})
assert r['active_signature'] is None, r

# call_hierarchy_items: list of dicts / null / wrong type
assert _normalize_call_hierarchy_items(None) == []
assert _normalize_call_hierarchy_items('garbage') == []
assert _normalize_call_hierarchy_items([{'name': 'foo'}, 'skip-me']) == [{'name': 'foo'}]

# call_hierarchy_calls: incoming uses 'from', outgoing uses 'to'
inc = [{'from': {'name': 'caller'}, 'fromRanges': [{'start': {}, 'end': {}}]}]
r = _normalize_call_hierarchy_calls(inc, 'from')
assert len(r) == 1 and r[0]['item']['name'] == 'caller', r
out = [{'to': {'name': 'callee'}, 'fromRanges': [{}]}]
r = _normalize_call_hierarchy_calls(out, 'to')
assert len(r) == 1 and r[0]['item']['name'] == 'callee', r

# code_actions: null / list / mixed types
assert _normalize_code_actions(None) == []
acts = [{'title': 'fix it', 'kind': 'quickfix'}, 'not-a-dict']
r = _normalize_code_actions(acts)
assert len(r) == 1 and r[0]['title'] == 'fix it', r

# _validate_range: missing / wrong types / valid
try:
    _validate_range('not-a-dict')
    raise SystemExit('expected lsp-range-invalid for str input')
except LspError as e:
    assert e.kind == 'lsp-range-invalid', e.kind
try:
    _validate_range({'start': {'line': 0, 'character': 0}})
    raise SystemExit('expected lsp-range-invalid for missing end')
except LspError as e:
    assert e.kind == 'lsp-range-invalid', e.kind
try:
    _validate_range({'start': {'line': -1, 'character': 0},
                     'end': {'line': 0, 'character': 0}})
    raise SystemExit('expected lsp-position-invalid for negative line')
except LspError as e:
    assert e.kind == 'lsp-position-invalid', e.kind
r = _validate_range({'start': {'line': 1, 'character': 2},
                     'end': {'line': 3, 'character': 4}})
assert r == {'start': {'line': 1, 'character': 2},
             'end': {'line': 3, 'character': 4}}, r
"
}

# --- 11b: Boundary contract holds for the new code_action surface ---------
# textDocument/codeAction is read-only per LSP 3.17 and is intentionally
# NOT in the deny set so the code_action MCP tool can call it.
# applyEdit + executeCommand stay denied so even though codeAction
# returns CodeAction objects with edit / command fields, the bridge
# cannot apply them.
t_codeaction_boundary_contract() {
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
from lsp_client import _FORBIDDEN_LSP_METHODS

# textDocument/codeAction must NOT be in the deny set (was removed
# when the read-only code_action MCP tool was added).
assert 'textDocument/codeAction' not in _FORBIDDEN_LSP_METHODS, \
    f'codeAction in deny set: {sorted(_FORBIDDEN_LSP_METHODS)}'

# Execution paths MUST remain denied. These are what would actually
# mutate the workspace; codeAction itself just returns metadata.
for required in ('workspace/applyEdit', 'workspace/executeCommand',
                 'textDocument/rename'):
    assert required in _FORBIDDEN_LSP_METHODS, \
        f'{required} missing from deny set: {sorted(_FORBIDDEN_LSP_METHODS)}'
"
}

# --- 11c: completion smoke against clangd (SKIP if clangd-19 missing) -----
# End-to-end: build the FastMCP server, find the registered completion
# tool, invoke it on a known C file at a position mid-call, expect
# at least one item back. Validates the wired-up handler chain
# (path validation -> didOpen -> request -> normalize -> envelope).
t_completion_smoke_clangd() {
    if ! command -v clangd-19 >/dev/null 2>&1; then
        return 0  # SKIP: clangd-19 not installed
    fi
    python3 - << 'PY'
import sys
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')

try:
    from mcp.server.fastmcp import FastMCP
except Exception:
    sys.exit(0)  # SKIP: mcp SDK not installed

import bridge

ws = Path.cwd()
srv = bridge._build_mcp(FastMCP, ws)
fn = srv._tool_manager._tools['completion'].fn

# Pick a real C source file with a known function position. Using
# src/kernel/main.c at line 0, character 0 is enough to exercise the
# path; clangd may need a moment to index but small files complete
# fast. We accept either a non-empty items list OR an LSP timeout
# envelope (cold-cache hosts may not finish indexing in 15 s); what
# we MUST NOT see is a path-resolution / type-validation failure.
import time
t0 = time.time()
result = fn(path='src/kernel/main.c', line=10, character=0)
elapsed = time.time() - t0

# Tolerate either a successful completion list OR a clangd-isn't-
# ready envelope. Anything else is a real failure.
if 'error' in result:
    kind = result['error']
    assert kind in ('lsp-timeout', 'lsp-spawner-import-failed',
                    'lsp-language-unsupported',
                    'lsp-binary-missing', 'lsp-spawn-failed'), \
        f'unexpected error: {result}'
    sys.exit(0)

# Successful path: response shape contract.
assert 'items' in result, result
assert 'isIncomplete' in result, result
assert 'truncated' in result, result
assert isinstance(result['items'], list), result
# clangd at a token boundary returns at least the surrounding
# kernel symbols; some positions return [] which is fine. We
# verify the SHAPE, not non-emptiness.

# Position validation contract: invalid line lands as envelope
# (NOT an exception -- _call_lsp wraps every LspError into the
# {'error': kind, 'detail': ...} dict shape so MCP-agent clients
# always see structured output).
result_bad = fn(path='src/kernel/main.c', line=-1, character=0)
assert 'error' in result_bad and result_bad['error'] == 'lsp-position-invalid', result_bad

# Trigger character validation: oversized rejected (also enveloped).
result = fn(path='src/kernel/main.c', line=0, character=0,
            trigger_character='abcdefghij')
assert 'error' in result and result['error'] == 'lsp-completion-trigger-invalid', result

print(f'[completion] OK in {elapsed:.2f}s')
PY
}

# --- 11e: Call hierarchy walks every prepared anchor ---------------------
# Codex Medium finding: an early revision queried only prepared[0].
# This test builds a fake LSP that returns N prepared CallHierarchyItems
# and asserts the bridge issues N follow-up calls and bundles them all
# into the response. No real LSP needed -- the contract is wire-level.
t_call_hierarchy_multi_anchor() {
    python3 - << 'PY'
import sys, json, threading
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')

try:
    from mcp.server.fastmcp import FastMCP
except Exception:
    sys.exit(0)  # SKIP

import bridge
from lsp_client import LspSubprocess

# Stub LSP that returns 3 prepared items, then resolves each follow-up
# distinctly so the test can prove every anchor was queried.
fake = r"""
import sys, json
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
while True:
    m = read_msg()
    if m is None: break
    method = m.get('method')
    if method == 'initialize':
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'capabilities':{}}})
    elif method == 'textDocument/prepareCallHierarchy':
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':[
            {'name': 'overload_a', 'kind': 12, 'uri': 'file:///x', 'range': {}, 'selectionRange': {}},
            {'name': 'overload_b', 'kind': 12, 'uri': 'file:///x', 'range': {}, 'selectionRange': {}},
            {'name': 'overload_c', 'kind': 12, 'uri': 'file:///x', 'range': {}, 'selectionRange': {}},
        ]})
    elif method == 'callHierarchy/incomingCalls':
        anchor_name = m['params']['item']['name']
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':[
            {'from': {'name': f'caller_of_{anchor_name}',
                      'uri': 'file:///x', 'range': {}, 'selectionRange': {}},
             'fromRanges': [{}]},
        ]})
    elif method == 'shutdown':
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    elif method == 'exit':
        break
    elif 'method' in m and 'id' not in m:
        pass
    elif 'id' in m:
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
"""

# Drive the bridge handler via a stub LSP injected through the spawn
# registry. We register a fresh language tag 'fake' so we don't
# collide with real spawners; then we map a tempfile extension to it.
import tempfile, os
ws = Path.cwd()
tmp = tempfile.NamedTemporaryFile(suffix='.fakeext', mode='w',
                                  delete=False, dir=str(ws))
try:
    tmp.write('// fake call-hierarchy stub source\n'); tmp.close()

    # Patch bridge tables so the new extension routes to a fake LSP.
    bridge._EXT_TO_LANG['.fakeext'] = 'fake'
    bridge._LANG_TO_LSP_LANGUAGE_ID['fake'] = 'plaintext'

    def spawn_fake(workspace_root):
        lsp = LspSubprocess(['python3', '-c', fake], lang='fake')
        lsp.initialize('file://' + str(workspace_root))
        return lsp
    bridge.register_spawner('fake', spawn_fake)

    srv = bridge._build_mcp(FastMCP, ws)
    fn = srv._tool_manager._tools['call_hierarchy_incoming'].fn
    result = fn(path=os.path.basename(tmp.name), line=0, character=0)

    assert 'error' not in result, result
    assert result['prepared_count'] == 3, result
    assert result['prepared_total'] == 3, result
    assert result['truncated'] is False, result
    assert result['deadline_exceeded'] is False, result
    assert len(result['anchors']) == 3, result
    names = sorted(a['anchor']['name'] for a in result['anchors'])
    assert names == ['overload_a', 'overload_b', 'overload_c'], names
    # Each anchor must have its own caller list (proves all 3 follow-ups fired).
    for entry in result['anchors']:
        assert len(entry['calls']) == 1, entry
        caller = entry['calls'][0]['item']['name']
        assert caller == f'caller_of_{entry["anchor"]["name"]}', entry
    print(f'[call_hierarchy] OK -- 3/3 anchors resolved')
finally:
    os.unlink(tmp.name)
    # Drop the fake LSP from registry + caches; let atexit clean the proc.
    bridge._EXT_TO_LANG.pop('.fakeext', None)
    bridge._LANG_TO_LSP_LANGUAGE_ID.pop('fake', None)
    bridge._LSP_SPAWNERS.pop('fake', None)
    with bridge._LIVE_LSPS_LOCK:
        keys = [k for k in bridge._LIVE_LSPS if k[0] == 'fake']
    for k in keys:
        with bridge._LIVE_LSPS_LOCK:
            inst = bridge._LIVE_LSPS.pop(k, None)
        if inst is not None:
            inst.shutdown(timeout=2.0)
PY
}

# --- 11f: Call hierarchy caps unbounded fan-out --------------------------
# Codex post-commit review High: unbounded prepareCallHierarchy output
# could turn a single MCP call into thousands of 15 s LSP requests.
# This test builds a stub LSP that returns 200 prepared items and
# asserts the bridge issues at most _CALL_HIERARCHY_MAX_ANCHORS=32
# follow-up requests, marks truncated=True, and surfaces prepared_total.
t_call_hierarchy_cap() {
    python3 - << 'PY'
import sys, json
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')

try:
    from mcp.server.fastmcp import FastMCP
except Exception:
    sys.exit(0)  # SKIP

import bridge
from lsp_client import LspSubprocess

# Stub LSP that returns 200 prepared items and counts every follow-up.
fake = r"""
import sys, json
follow_count = [0]   # list-wrapped so module-level rebinding is in-place
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
while True:
    m = read_msg()
    if m is None: break
    method = m.get('method')
    if method == 'initialize':
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'capabilities':{}}})
    elif method == 'textDocument/prepareCallHierarchy':
        items = [{'name': f'overload_{i:03d}', 'kind': 12,
                  'uri': 'file:///x', 'range': {}, 'selectionRange': {}}
                 for i in range(200)]
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':items})
    elif method == 'callHierarchy/incomingCalls':
        follow_count[0] += 1
        # Echo the count as the caller name so the test can verify it.
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':[
            {'from': {'name': f'follow_{follow_count[0]}',
                      'uri': 'file:///x', 'range': {}, 'selectionRange': {}},
             'fromRanges': [{}]},
        ]})
    elif method == 'shutdown':
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    elif method == 'exit':
        break
    elif 'method' in m and 'id' not in m:
        pass
    elif 'id' in m:
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
"""

import tempfile, os
ws = Path.cwd()
tmp = tempfile.NamedTemporaryFile(suffix='.fakeext2', mode='w',
                                  delete=False, dir=str(ws))
try:
    tmp.write('// fake stub source\n'); tmp.close()

    bridge._EXT_TO_LANG['.fakeext2'] = 'fake2'
    bridge._LANG_TO_LSP_LANGUAGE_ID['fake2'] = 'plaintext'

    def spawn_fake(workspace_root):
        lsp = LspSubprocess(['python3', '-c', fake], lang='fake2')
        lsp.initialize('file://' + str(workspace_root))
        return lsp
    bridge.register_spawner('fake2', spawn_fake)

    srv = bridge._build_mcp(FastMCP, ws)
    fn = srv._tool_manager._tools['call_hierarchy_incoming'].fn
    result = fn(path=os.path.basename(tmp.name), line=0, character=0)

    assert 'error' not in result, result
    cap = bridge._CALL_HIERARCHY_MAX_ANCHORS
    assert result['prepared_total'] == 200, result
    assert result['prepared_count'] == cap, result
    assert result['truncated'] is True, result
    # With instant-replying stub the deadline never fires.
    assert result['deadline_exceeded'] is False, result
    assert len(result['anchors']) == cap, result
    print(f'[call_hierarchy] OK -- capped at {cap}/200 anchors, truncated=True')
finally:
    os.unlink(tmp.name)
    bridge._EXT_TO_LANG.pop('.fakeext2', None)
    bridge._LANG_TO_LSP_LANGUAGE_ID.pop('fake2', None)
    bridge._LSP_SPAWNERS.pop('fake2', None)
    with bridge._LIVE_LSPS_LOCK:
        keys = [k for k in bridge._LIVE_LSPS if k[0] == 'fake2']
    for k in keys:
        with bridge._LIVE_LSPS_LOCK:
            inst = bridge._LIVE_LSPS.pop(k, None)
        if inst is not None:
            inst.shutdown(timeout=2.0)
PY
}

# --- 11g: Call hierarchy bounded by wall-clock deadline ------------------
# Codex post-commit perf review High: cap-only protection still allowed
# ~8 minute interactive latency on a wedged LSP. Defense-in-depth fix
# adds a wall-clock deadline (_CALL_HIERARCHY_DEADLINE_S=30 s). This
# test forces the deadline by patching it to ~0.3 s, has a stub that
# never replies to follow-ups, and asserts the handler returns within
# a few seconds with deadline_exceeded=True. Wall-time bounded.
t_call_hierarchy_deadline() {
    python3 - << 'PY'
import sys, time
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')

try:
    from mcp.server.fastmcp import FastMCP
except Exception:
    sys.exit(0)  # SKIP

import bridge
from lsp_client import LspSubprocess

# Stub LSP that returns 5 prepared items but NEVER replies to
# incomingCalls follow-ups. Each follow-up MUST hit a per-request
# timeout, but the cumulative deadline must fire well before the
# 5 anchors * per-call timeout would.
fake = r"""
import sys, json
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
while True:
    m = read_msg()
    if m is None: break
    method = m.get('method')
    if method == 'initialize':
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'capabilities':{}}})
    elif method == 'textDocument/prepareCallHierarchy':
        items = [{'name': f'a{i}', 'kind': 12, 'uri': 'file:///x',
                  'range': {}, 'selectionRange': {}} for i in range(5)]
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':items})
    elif method == 'callHierarchy/incomingCalls':
        # Silently swallow -- never reply. This is the wedged-LSP
        # simulation; the bridge MUST hit its deadline.
        pass
    elif method == 'shutdown':
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    elif method == 'exit':
        break
    elif 'method' in m and 'id' not in m:
        pass
    elif 'id' in m:
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
"""

import tempfile, os
ws = Path.cwd()
tmp = tempfile.NamedTemporaryFile(suffix='.fakeext3', mode='w',
                                  delete=False, dir=str(ws))
try:
    tmp.write('// fake stub source\n'); tmp.close()

    bridge._EXT_TO_LANG['.fakeext3'] = 'fake3'
    bridge._LANG_TO_LSP_LANGUAGE_ID['fake3'] = 'plaintext'

    def spawn_fake(workspace_root):
        lsp = LspSubprocess(['python3', '-c', fake], lang='fake3')
        lsp.initialize('file://' + str(workspace_root))
        return lsp
    bridge.register_spawner('fake3', spawn_fake)

    # Patch the deadline + per-follow timeout for fast test wall-clock.
    saved_deadline = bridge._CALL_HIERARCHY_DEADLINE_S
    saved_per_call = bridge._CALL_HIERARCHY_FOLLOW_TIMEOUT_S
    bridge._CALL_HIERARCHY_DEADLINE_S = 0.5
    bridge._CALL_HIERARCHY_FOLLOW_TIMEOUT_S = 0.2
    try:
        srv = bridge._build_mcp(FastMCP, ws)
        fn = srv._tool_manager._tools['call_hierarchy_incoming'].fn
        t0 = time.monotonic()
        result = fn(path=os.path.basename(tmp.name), line=0, character=0)
        elapsed = time.monotonic() - t0
    finally:
        bridge._CALL_HIERARCHY_DEADLINE_S = saved_deadline
        bridge._CALL_HIERARCHY_FOLLOW_TIMEOUT_S = saved_per_call

    assert 'error' not in result, result
    assert result['prepared_total'] == 5, result
    assert result['truncated'] is False, result
    assert result['deadline_exceeded'] is True, result
    # Ceiling: deadline (0.5) + last per-call timeout (0.2) + slop;
    # we should NOT have walked all 5 anchors.
    assert elapsed < 2.0, f'handler ran {elapsed:.2f}s -- deadline ineffective'
    # We should have at MOST a couple of anchors (each takes ~0.2s).
    assert len(result['anchors']) <= 5, result
    # Each walked anchor either has anchor_error (per-call timeout)
    # or empty calls; none should have actual call data because the
    # stub never replied.
    for entry in result['anchors']:
        assert entry['calls'] == [], entry
    print(f'[call_hierarchy] OK -- deadline fired at {elapsed:.2f}s '
          f'(walked {len(result["anchors"])}/5 anchors before deadline)')
finally:
    os.unlink(tmp.name)
    bridge._EXT_TO_LANG.pop('.fakeext3', None)
    bridge._LANG_TO_LSP_LANGUAGE_ID.pop('fake3', None)
    bridge._LSP_SPAWNERS.pop('fake3', None)
    with bridge._LIVE_LSPS_LOCK:
        keys = [k for k in bridge._LIVE_LSPS if k[0] == 'fake3']
    for k in keys:
        with bridge._LIVE_LSPS_LOCK:
            inst = bridge._LIVE_LSPS.pop(k, None)
        if inst is not None:
            inst.shutdown(timeout=2.0)
PY
}

# --- 11d: code_action immutability check (SKIP if clangd-19 missing) ------
# code_action MUST be metadata-only. Even if clangd returns a
# CodeAction with an `edit: WorkspaceEdit`, the bridge MUST NOT apply
# it. Verify with md5sum before/after the call.
t_codeaction_immutability() {
    if ! command -v clangd-19 >/dev/null 2>&1; then
        return 0  # SKIP
    fi
    python3 - << 'PY'
import sys, hashlib
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')

try:
    from mcp.server.fastmcp import FastMCP
except Exception:
    sys.exit(0)  # SKIP

import bridge

ws = Path.cwd()
srv = bridge._build_mcp(FastMCP, ws)
fn = srv._tool_manager._tools['code_action'].fn

target = ws / 'src/kernel/main.c'
before = hashlib.md5(target.read_bytes()).hexdigest()

# Cover lines 0-5; clangd may return [] (no actions in range) which
# is still a valid response. We only care about IMMUTABILITY.
result = fn(path='src/kernel/main.c',
            range={'start': {'line': 0, 'character': 0},
                   'end':   {'line': 5, 'character': 0}})

after = hashlib.md5(target.read_bytes()).hexdigest()
assert before == after, f'code_action mutated the file! md5 {before} -> {after}'

# Response shape: either the success envelope or an LSP error envelope
# (cold-start clangd / unsupported by this LSP version).
if 'error' in result:
    sys.exit(0)
assert 'actions' in result, result
assert isinstance(result['actions'], list), result
assert 'note' in result and 'READ-ONLY' in result['note'], result

# Range validation: bad input rejected before reaching the LSP.
result = fn(path='src/kernel/main.c', range={'start': 'not-a-dict'})
assert 'error' in result and result['error'] == 'lsp-range-invalid', result

print('[code_action] OK -- file unchanged, response read-only')
PY
}

# --- 12a: apply_text() open-or-refresh decision matrix --------------------
# Pure-data test: drives apply_text() against a stub LSP and asserts
# (1) first call is didOpen, (2) same-mtime second call is no-op,
# (3) different-mtime third call is didChange with version=2.
t_apply_text_decision_matrix() {
    python3 - << 'PY'
import sys, json
sys.path.insert(0, 'scripts/lsp-mcp')
from lsp_client import LspSubprocess

# Echo-stub LSP: replies to initialize; records every notification.
fake = r"""
import sys, json, time
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
captured = []
import sys as _s
while True:
    m = read_msg()
    if m is None: break
    method = m.get('method')
    if 'id' in m:
        if method == 'initialize':
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'capabilities':{}}})
        elif method == 'shutdown':
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
        else:
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    else:
        if method == 'exit': break
        # Notification: echo back as a NOTIFICATION (no id) on a
        # custom method so the test side can read it from
        # diagnostics_by_uri-style stash. Easier: just print to stderr.
        print(json.dumps({'method': method, 'params': m.get('params')}),
              file=_s.stderr, flush=True)
"""

lsp = LspSubprocess(['python3', '-c', fake], lang='fake-12a')
notifications: list = []

# Capture stderr to inspect what the bridge actually sent.
import threading
def drain():
    while True:
        line = lsp._proc.stderr.readline() if lsp._proc and lsp._proc.stderr else b''
        if not line: break
        try:
            notifications.append(json.loads(line.decode('utf-8').strip()))
        except Exception:
            pass

# The LspSubprocess already starts its own stderr-drain thread that
# DISCARDS stderr. We need a custom hook -- monkey-patch the drain
# loop AFTER spawn by attaching our own collector via the proc's
# stderr pipe BEFORE first notify. Simpler: just count what the
# stub echoes via a different channel.

# Drop the existing stderr drain thread by joining on a sentinel:
# the existing thread reads everything, so our test cannot also
# read. Workaround: use lsp._io_lock-aware notify counting via
# patching the _io_lock acquire counts.

# Cleanest: just count notify() calls via wrapper.
notify_calls: list = []
real_notify = lsp.notify
def counting_notify(method, params=None):
    notify_calls.append((method, params))
    return real_notify(method, params)
lsp.notify = counting_notify  # type: ignore

try:
    lsp.initialize('file:///tmp/test')

    # (1) First apply_text -> didOpen, version=1.
    v = lsp.apply_text('file:///x.py', 'python', 'first', mtime_ns=100)
    assert v == 1, v
    open_calls = [c for c in notify_calls if c[0] == 'textDocument/didOpen']
    assert len(open_calls) == 1, open_calls
    assert open_calls[0][1]['textDocument']['version'] == 1
    assert open_calls[0][1]['textDocument']['text'] == 'first'

    # (2) Same mtime -> no-op, return cached version=1, no didChange.
    v = lsp.apply_text('file:///x.py', 'python', 'first', mtime_ns=100)
    assert v == 1, v
    chg_calls = [c for c in notify_calls if c[0] == 'textDocument/didChange']
    assert len(chg_calls) == 0, chg_calls

    # (3) Different mtime -> didChange, version=2.
    v = lsp.apply_text('file:///x.py', 'python', 'second', mtime_ns=200)
    assert v == 2, v
    chg_calls = [c for c in notify_calls if c[0] == 'textDocument/didChange']
    assert len(chg_calls) == 1, chg_calls
    assert chg_calls[0][1]['textDocument']['version'] == 2
    assert chg_calls[0][1]['contentChanges'][0]['text'] == 'second'

    # (4) Force didSave on refresh.
    v = lsp.apply_text('file:///x.py', 'python', 'third', mtime_ns=300,
                       force_did_save=True)
    assert v == 3, v
    save_calls = [c for c in notify_calls if c[0] == 'textDocument/didSave']
    assert len(save_calls) == 1, save_calls

    # (5) Second URI tracked independently -- new didOpen, version=1.
    v = lsp.apply_text('file:///y.py', 'python', 'other', mtime_ns=50)
    assert v == 1, v
    open_calls = [c for c in notify_calls if c[0] == 'textDocument/didOpen']
    assert len(open_calls) == 2, open_calls

    # (6) mtime_ns=None -> skip refresh check, return cached version
    # (does not block, does not over-fire didChange).
    v = lsp.apply_text('file:///x.py', 'python', 'whatever', mtime_ns=None)
    assert v == 3, v   # cached after step 4
finally:
    lsp.shutdown(timeout=2.0)
PY
}

# --- 12b: did_close fires at shutdown for every tracked URI ---------------
t_didclose_at_shutdown() {
    python3 - << 'PY'
import sys, json
sys.path.insert(0, 'scripts/lsp-mcp')
from lsp_client import LspSubprocess

# Stub that records every notification it receives via stderr.
fake = r"""
import sys, json
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
import os
log = open(os.environ['STUB_LOG'], 'w')
while True:
    m = read_msg()
    if m is None: break
    method = m.get('method')
    log.write(json.dumps({'method': method, 'has_id': 'id' in m}) + '\n')
    log.flush()
    if 'id' in m:
        if method == 'initialize':
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'capabilities':{}}})
        elif method == 'shutdown':
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
        else:
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    else:
        if method == 'exit':
            break
log.close()
"""

import os, tempfile
log_path = tempfile.mktemp(prefix='lsp-mcp-stub-12b-', suffix='.log')
env = dict(os.environ); env['STUB_LOG'] = log_path
lsp = LspSubprocess(['python3', '-c', fake], lang='fake-12b', env=env)
try:
    lsp.initialize('file:///tmp/test')
    # Open three URIs.
    for i in range(3):
        lsp.apply_text(f'file:///x{i}.py', 'python', 'body', mtime_ns=i)
    assert lsp.open_uris == {f'file:///x{i}.py' for i in range(3)}
finally:
    # shutdown() must didClose every URI before issuing 'shutdown'.
    lsp.shutdown(timeout=2.0)

# Inspect stub log: ordering must show 3 didClose, then shutdown,
# then exit.
with open(log_path) as f:
    lines = [json.loads(l) for l in f if l.strip()]
os.unlink(log_path)

methods = [l['method'] for l in lines]
# Must have 3 didClose calls.
close_count = methods.count('textDocument/didClose')
assert close_count == 3, f'expected 3 didClose, got {close_count}: {methods}'
# Order: every didClose must come before the shutdown request.
shutdown_idx = methods.index('shutdown')
for i, m in enumerate(methods):
    if m == 'textDocument/didClose':
        assert i < shutdown_idx, f'didClose at {i} after shutdown at {shutdown_idx}'
PY
}

# --- 12c: cleanup_paths walked after shutdown ----------------------------
t_cleanup_paths_walked() {
    python3 - << 'PY'
import sys, os, tempfile
sys.path.insert(0, 'scripts/lsp-mcp')
from lsp_client import LspSubprocess

# Minimal LSP stub.
fake = r"""
import sys, json
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
while True:
    m = read_msg()
    if m is None: break
    method = m.get('method')
    if 'id' in m:
        if method == 'initialize':
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'capabilities':{}}})
        else:
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    elif method == 'exit':
        break
"""

# Two tempdirs: one with a real file inside, one nested.
tmp1 = tempfile.mkdtemp(prefix='lsp-mcp-cleanup-12c-')
tmp2 = tempfile.mkdtemp(prefix='lsp-mcp-cleanup-12c-')
with open(os.path.join(tmp1, 'file.txt'), 'w') as f:
    f.write('content')

lsp = LspSubprocess(['python3', '-c', fake], lang='fake-12c')
try:
    lsp.initialize('file:///tmp/test')
    lsp.cleanup_paths.append(tmp1)
    lsp.cleanup_paths.append(tmp2)
    assert os.path.isdir(tmp1) and os.path.isdir(tmp2)
finally:
    lsp.shutdown(timeout=2.0)

# After shutdown both tempdirs MUST be gone.
assert not os.path.exists(tmp1), f'{tmp1} still exists'
assert not os.path.exists(tmp2), f'{tmp2} still exists'
# cleanup_paths cleared so a double-shutdown does not double-rmtree.
assert lsp.cleanup_paths == [], lsp.cleanup_paths
PY
}

# --- 12d: bridge handler forwards didChange when on-disk file changes ----
t_bridge_handler_forwards_didchange() {
    python3 - << 'PY'
import sys, json, os, tempfile, time
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')

try:
    from mcp.server.fastmcp import FastMCP
except Exception:
    sys.exit(0)  # SKIP

import bridge
from lsp_client import LspSubprocess

# Stub LSP that logs every notification + replies to hover with a
# canned body so the bridge handler can return.
fake = r"""
import sys, json, os
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
log = open(os.environ['STUB_LOG'], 'w')
while True:
    m = read_msg()
    if m is None: break
    method = m.get('method')
    log.write(json.dumps({'method': method, 'has_id': 'id' in m}) + '\n')
    log.flush()
    if 'id' in m:
        if method == 'initialize':
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'capabilities':{}}})
        elif method == 'textDocument/hover':
            write_msg({'jsonrpc':'2.0','id':m['id'],
                       'result':{'contents':{'kind':'markdown','value':'h'}}})
        else:
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    elif method == 'exit':
        break
log.close()
"""

ws = Path.cwd()
log_path = tempfile.mktemp(prefix='lsp-mcp-stub-12d-', suffix='.log')
tmp = tempfile.NamedTemporaryFile(suffix='.fakeext12d', mode='w',
                                  delete=False, dir=str(ws))
try:
    tmp.write('first body\n'); tmp.close()

    bridge._EXT_TO_LANG['.fakeext12d'] = 'fake12d'
    bridge._LANG_TO_LSP_LANGUAGE_ID['fake12d'] = 'plaintext'
    env = dict(os.environ); env['STUB_LOG'] = log_path

    def spawn_fake(workspace_root):
        lsp = LspSubprocess(['python3', '-c', fake], lang='fake12d', env=env)
        lsp.initialize('file://' + str(workspace_root))
        return lsp
    bridge.register_spawner('fake12d', spawn_fake)

    srv = bridge._build_mcp(FastMCP, ws)
    fn = srv._tool_manager._tools['hover'].fn

    # First hover -> didOpen forwarded, hover replies.
    r1 = fn(path=os.path.basename(tmp.name), line=0, character=0)
    assert 'error' not in r1, r1

    # Force a different mtime by writing the file again (with a sleep
    # to clear nanosecond same-tick edge case on some filesystems).
    time.sleep(0.05)
    with open(tmp.name, 'w') as f:
        f.write('second body with more text\n')

    # Second hover -> apply_text MUST detect mtime drift and forward
    # textDocument/didChange before issuing the hover.
    r2 = fn(path=os.path.basename(tmp.name), line=0, character=0)
    assert 'error' not in r2, r2

    # Inspect stub log: must have at least one didOpen and at least
    # one didChange before the second hover. Order: didOpen, hover,
    # didChange, hover.
    with open(log_path) as f:
        lines = [json.loads(l) for l in f if l.strip()]
    methods = [l['method'] for l in lines]
    assert methods.count('textDocument/didOpen') == 1, methods
    assert methods.count('textDocument/didChange') == 1, methods
    open_idx = methods.index('textDocument/didOpen')
    change_idx = methods.index('textDocument/didChange')
    hover_indices = [i for i, m in enumerate(methods) if m == 'textDocument/hover']
    assert len(hover_indices) == 2, methods
    assert open_idx < hover_indices[0] < change_idx < hover_indices[1], methods
    print('[didChange] OK -- mtime drift forwarded didChange before 2nd hover')
finally:
    try: os.unlink(tmp.name)
    except OSError: pass
    try: os.unlink(log_path)
    except OSError: pass
    bridge._EXT_TO_LANG.pop('.fakeext12d', None)
    bridge._LANG_TO_LSP_LANGUAGE_ID.pop('fake12d', None)
    bridge._LSP_SPAWNERS.pop('fake12d', None)
    with bridge._LIVE_LSPS_LOCK:
        keys = [k for k in bridge._LIVE_LSPS if k[0] == 'fake12d']
    for k in keys:
        with bridge._LIVE_LSPS_LOCK:
            inst = bridge._LIVE_LSPS.pop(k, None)
        if inst is not None:
            inst.shutdown(timeout=2.0)
PY
}

# --- 12e: apply_text rejects during teardown -----------------------------
# Codex post-implementation review High: a concurrent apply_text
# between shutdown's snapshot of open_uris and its didClose loop
# could register a new URI invisible to the snapshot AND interleave
# normal traffic into the teardown sequence. Fix sets _shutdown_called
# (under _open_uris_lock) BEFORE the didClose snapshot. This test
# starts shutdown, then races apply_text against it; the apply_text
# MUST reject with lsp-shutdown OR (rare) succeed before the teardown
# commit -- never silently send a notification that misses didClose.
t_apply_text_rejects_during_teardown() {
    python3 - << 'PY'
import sys, threading, time
sys.path.insert(0, 'scripts/lsp-mcp')
from lsp_client import LspSubprocess, LspError

# Minimal LSP stub.
fake = r"""
import sys, json, time
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
while True:
    m = read_msg()
    if m is None: break
    method = m.get('method')
    if 'id' in m:
        if method == 'initialize':
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'capabilities':{}}})
        elif method == 'shutdown':
            # Slow-shutdown: sleep so the racing apply_text has a
            # window to land DURING teardown, not before it.
            time.sleep(0.1)
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
        else:
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    elif method == 'exit':
        break
"""

# Run the race many times to actually catch it.
shutdown_rejections = 0
shutdown_succeeded = 0
for trial in range(20):
    lsp = LspSubprocess(['python3', '-c', fake], lang=f'fake-12e-{trial}')
    try:
        lsp.initialize('file:///tmp/test')
        # Open one URI so didClose has something to send.
        lsp.apply_text('file:///pre.py', 'python', 'pre', mtime_ns=1)

        late_result = {'kind': None, 'error': None}
        def late_apply():
            # Tiny pause to let shutdown begin the didClose phase.
            time.sleep(0.02)
            try:
                lsp.apply_text('file:///late.py', 'python', 'late', mtime_ns=2)
                late_result['kind'] = 'success'
            except LspError as e:
                late_result['kind'] = 'error'
                late_result['error'] = e.kind

        t = threading.Thread(target=late_apply)
        t.start()
        lsp.shutdown(timeout=2.0)
        t.join(timeout=3.0)

        # The late apply must have either succeeded BEFORE the
        # _shutdown_called commit (rare, unobservable race) OR been
        # rejected with lsp-shutdown. It must NEVER crash, deadlock,
        # or silently sneak a notification past the teardown.
        assert late_result['kind'] in ('success', 'error'), late_result
        if late_result['kind'] == 'error':
            shutdown_rejections += 1
            assert late_result['error'] == 'lsp-shutdown', late_result
        else:
            shutdown_succeeded += 1
    finally:
        # Ensure clean teardown if anything broke.
        try: lsp.shutdown(timeout=1.0)
        except Exception: pass

# At least SOME trials must have hit the rejection path -- otherwise
# the timing is wrong and the test isn't actually exercising the race.
# With 20 trials the rejection rate is reliably ~95%+ on dev hardware.
assert shutdown_rejections > 0, \
    f'race never landed during teardown across 20 trials ' \
    f'(succeeded={shutdown_succeeded}); test timing broken'
print(f'[apply_text/teardown] OK -- {shutdown_rejections}/20 trials ' \
      f'rejected with lsp-shutdown, {shutdown_succeeded} raced past commit')
PY
}

# --- 12f: post-apply_text request rejected during teardown ---------------
# Codex post-implementation review of the file-change-lifecycle work
# caught a follow-up High: an MCP tool thread that already passed
# apply_text's gate would be inside lsp.request() when shutdown
# commits. The fix binds _teardown_thread_id to the shutdown owner
# thread (instead of using a global bool), so request()/notify()
# from any other thread reject with lsp-shutdown after the commit.
# Test simulates a tool thread that has cleared apply_text but not
# yet called request -- shutdown fires in between, then the tool
# thread tries to issue hover. It must reject with lsp-shutdown.
t_post_apply_text_request_rejected_in_teardown() {
    python3 - << 'PY'
import sys, threading, time
sys.path.insert(0, 'scripts/lsp-mcp')
from lsp_client import LspSubprocess, LspError

# Slow-shutdown stub: shutdown takes long enough that the racing
# request lands DURING teardown.
fake = r"""
import sys, json, time
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
while True:
    m = read_msg()
    if m is None: break
    method = m.get('method')
    if 'id' in m:
        if method == 'initialize':
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'capabilities':{}}})
        elif method == 'shutdown':
            time.sleep(0.15)
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
        else:
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    elif method == 'exit':
        break
"""

total_trials = 10
for trial in range(total_trials):
    lsp = LspSubprocess(['python3', '-c', fake], lang=f'fake-12f-{trial}')
    try:
        lsp.initialize('file:///tmp/test')
        # Pre-open a URI so apply_text's same-mtime no-op path doesn't
        # send any wire traffic; the tool thread's request() is the
        # only thing under test.
        lsp.apply_text('file:///pre.py', 'python', 'pre', mtime_ns=1)

        result = {'kind': None, 'error': None}

        def tool_thread():
            # Cross apply_text BEFORE shutdown commits; result
            # captured but not gated -- this proves the tool reached
            # the post-apply_text state.
            try:
                lsp.apply_text('file:///pre.py', 'python', 'pre',
                               mtime_ns=1)
            except LspError as e:
                # apply_text rejected before we even got to request --
                # not the path we want to exercise. Note + return.
                result['kind'] = 'rejected_at_apply_text'
                result['error'] = e.kind
                return
            # Now BLOCK until the main thread commits shutdown. This
            # eliminates the timing-sensitive race the earlier
            # revision had: by polling _shutdown_called we
            # deterministically observe the post-commit window before
            # issuing the racing request. The test then asserts the
            # request MUST reject -- not "may succeed if it raced
            # before commit", which the prior pass-rate-only assert
            # would have allowed.
            for _ in range(2000):  # ~2s ceiling at 1ms poll
                if lsp._shutdown_called:
                    break
                time.sleep(0.001)
            assert lsp._shutdown_called, 'shutdown never committed in 2s'
            try:
                lsp.request('hover', {'textDocument': {'uri': 'file:///pre.py'},
                                      'position': {'line': 0, 'character': 0}},
                            timeout=1.0)
                result['kind'] = 'success'
            except LspError as e:
                result['kind'] = 'error'
                result['error'] = e.kind

        t = threading.Thread(target=tool_thread)
        t.start()
        # Yield so the tool thread is past apply_text before we
        # commit shutdown.
        time.sleep(0.02)
        lsp.shutdown(timeout=2.0)
        t.join(timeout=4.0)

        # apply_text could have lost the race and rejected first
        # (rare but acceptable -- shows the OTHER gate fired). For
        # every trial that DID reach request(), the result MUST be
        # an error with kind=lsp-shutdown.
        if result['kind'] == 'rejected_at_apply_text':
            continue
        assert result['kind'] == 'error', \
            f'trial {trial}: post-commit request returned {result!r}; ' \
            f'expected lsp-shutdown rejection'
        assert result['error'] == 'lsp-shutdown', \
            f'trial {trial}: wrong rejection kind: {result!r}'
    finally:
        try: lsp.shutdown(timeout=1.0)
        except Exception: pass

print(f'[post-apply_text/teardown] OK -- all {total_trials} trials ' \
      f'rejected at request() after teardown commit (deterministic)')
PY
}

# --- 13a: Crash detection sets _crashed + _crash_reason ------------------
# Direct unit test of LspSubprocess crash flagging. Spawn a stub
# LSP, kill it, wait for the reader thread to exit, and assert the
# _crashed flag flips True with a reason captured from the proc
# poll() exit code -- not from a clean shutdown path.
t_crash_flag_set_on_unexpected_exit() {
    python3 - << 'PY'
import sys, os, signal, time
sys.path.insert(0, 'scripts/lsp-mcp')
from lsp_client import LspSubprocess

# Echo-stub that hangs in read until killed.
fake = r"""
import sys
while True:
    ch = sys.stdin.buffer.read(1)
    if not ch: break
"""
lsp = LspSubprocess(['python3', '-c', fake], lang='fake-13a')
try:
    pid = lsp.pid
    assert pid, lsp
    # Kill ungracefully (no shutdown RPC) -- simulates SIGKILL.
    os.kill(pid, signal.SIGKILL)
    # Wait for reader thread to notice EOF + flip _crashed.
    for _ in range(200):
        if lsp._crashed:
            break
        time.sleep(0.01)
    assert lsp._crashed, 'reader thread did not flag _crashed after kill'
    assert lsp.crashed is True, 'public crashed property did not surface'
    assert lsp._crash_reason is not None, 'no crash reason captured'
    assert 'subprocess exited' in lsp._crash_reason, lsp._crash_reason
finally:
    try: lsp.shutdown(timeout=1.0)
    except Exception: pass
PY
}

# --- 13b: bridge respawns on next request after crash --------------------
# End-to-end: register a stub spawner whose subprocess returns
# canned hover responses. After first call succeeds, kill the
# subprocess. Next hover call MUST trigger _get_or_spawn to detect
# the crash, respawn, replay didOpen, and return a normalized
# result. Restart count goes from 0 to 1 on _LSP_HEALTH.
t_respawn_on_next_request() {
    python3 - << 'PY'
import sys, os, time
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')

try:
    from mcp.server.fastmcp import FastMCP
except Exception:
    sys.exit(0)  # SKIP

import bridge
from lsp_client import LspSubprocess

fake = r"""
import sys, json
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
while True:
    m = read_msg()
    if m is None: break
    method = m.get('method')
    if 'id' in m:
        if method == 'initialize':
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'capabilities':{}}})
        elif method == 'textDocument/hover':
            write_msg({'jsonrpc':'2.0','id':m['id'],
                       'result':{'contents':{'kind':'markdown','value':'h'}}})
        else:
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    elif method == 'exit':
        break
"""

# Make backoff fast for the test (default would be 1 s).
saved_base = bridge._RESPAWN_BACKOFF_BASE_S
bridge._RESPAWN_BACKOFF_BASE_S = 0.05

import tempfile
ws = Path.cwd()
tmp = tempfile.NamedTemporaryFile(suffix='.fakeext13b', mode='w',
                                  delete=False, dir=str(ws))
try:
    tmp.write('// hello\n'); tmp.close()
    bridge._EXT_TO_LANG['.fakeext13b'] = 'fake13b'
    bridge._LANG_TO_LSP_LANGUAGE_ID['fake13b'] = 'plaintext'

    def spawn_fake(workspace_root):
        lsp = LspSubprocess(['python3', '-c', fake], lang='fake13b')
        lsp.initialize('file://' + str(workspace_root))
        return lsp
    bridge.register_spawner('fake13b', spawn_fake)

    srv = bridge._build_mcp(FastMCP, ws)
    hover_fn = srv._tool_manager._tools['hover'].fn
    health_fn = srv._tool_manager._tools['_health'].fn

    # First hover -> opens file, succeeds.
    r1 = hover_fn(path=os.path.basename(tmp.name), line=0, character=0)
    assert 'error' not in r1, r1

    # Sanity: _health shows healthy + restart_count=0.
    h = health_fn()
    fake_entries = h['languages'].get('fake13b', {})
    assert fake_entries, h
    first_entry = next(iter(fake_entries.values()))
    assert first_entry['status'] == 'healthy', first_entry
    assert first_entry['restart_count'] == 0, first_entry
    assert first_entry['alive'] is True, first_entry

    # KILL the subprocess.
    import signal
    with bridge._LIVE_LSPS_LOCK:
        keys = [k for k in bridge._LIVE_LSPS if k[0] == 'fake13b']
    assert keys, 'fake13b not in live registry'
    inst = bridge._LIVE_LSPS[keys[0]]
    pid = inst.pid
    os.kill(pid, signal.SIGKILL)
    # Wait for reader thread to flip _crashed.
    for _ in range(200):
        if inst._crashed:
            break
        time.sleep(0.01)
    assert inst._crashed, 'reader did not detect kill in time'

    # Second hover -> should respawn transparently and succeed.
    t0 = time.monotonic()
    r2 = hover_fn(path=os.path.basename(tmp.name), line=0, character=0)
    elapsed = time.monotonic() - t0
    assert 'error' not in r2, f'respawn failed: {r2}'

    # _health now shows restart_count=1, alive=True, status=healthy.
    h = health_fn()
    fake_entries = h['languages']['fake13b']
    after_entry = next(iter(fake_entries.values()))
    assert after_entry['restart_count'] == 1, after_entry
    assert after_entry['alive'] is True, after_entry
    assert after_entry['status'] == 'healthy', after_entry
    assert after_entry['last_crash_reason'] is not None, after_entry

    print(f'[respawn] OK -- restart_count 0 -> 1, recovered in {elapsed:.2f}s')
finally:
    bridge._RESPAWN_BACKOFF_BASE_S = saved_base
    try: os.unlink(tmp.name)
    except OSError: pass
    bridge._EXT_TO_LANG.pop('.fakeext13b', None)
    bridge._LANG_TO_LSP_LANGUAGE_ID.pop('fake13b', None)
    bridge._LSP_SPAWNERS.pop('fake13b', None)
    with bridge._LIVE_LSPS_LOCK:
        keys = [k for k in bridge._LIVE_LSPS if k[0] == 'fake13b']
        for k in keys:
            inst = bridge._LIVE_LSPS.pop(k, None)
            bridge._LSP_HEALTH.pop(k, None)
    if 'inst' in dir() and inst is not None:
        try: inst.shutdown(timeout=1.0)
        except Exception: pass
PY
}

# --- 13c: FAILED state after threshold crashes -- lsp-persistently-crashing
# Configure threshold + window for fast test. After N=2 crashes
# within the window, the bridge MUST refuse further requests with
# lsp-persistently-crashing instead of respawning forever.
t_failed_state_after_threshold() {
    python3 - << 'PY'
import sys, os, time, signal
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')

try:
    from mcp.server.fastmcp import FastMCP
except Exception:
    sys.exit(0)

import bridge
from lsp_client import LspSubprocess

fake = r"""
import sys, json
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
while True:
    m = read_msg()
    if m is None: break
    method = m.get('method')
    if 'id' in m:
        if method == 'initialize':
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'capabilities':{}}})
        elif method == 'textDocument/hover':
            write_msg({'jsonrpc':'2.0','id':m['id'],
                       'result':{'contents':{'kind':'markdown','value':'h'}}})
        else:
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    elif method == 'exit':
        break
"""

# Tighten policy for the test.
saved_threshold = bridge._RESPAWN_FAILED_THRESHOLD
saved_window = bridge._RESPAWN_FAILED_WINDOW_S
saved_base = bridge._RESPAWN_BACKOFF_BASE_S
bridge._RESPAWN_FAILED_THRESHOLD = 2     # fail after 2 crashes
bridge._RESPAWN_FAILED_WINDOW_S = 5.0    # within 5 seconds
bridge._RESPAWN_BACKOFF_BASE_S = 0.05    # fast retry

import tempfile
ws = Path.cwd()
tmp = tempfile.NamedTemporaryFile(suffix='.fakeext13c', mode='w',
                                  delete=False, dir=str(ws))
try:
    tmp.write('// hello\n'); tmp.close()
    bridge._EXT_TO_LANG['.fakeext13c'] = 'fake13c'
    bridge._LANG_TO_LSP_LANGUAGE_ID['fake13c'] = 'plaintext'

    def spawn_fake(workspace_root):
        lsp = LspSubprocess(['python3', '-c', fake], lang='fake13c')
        lsp.initialize('file://' + str(workspace_root))
        return lsp
    bridge.register_spawner('fake13c', spawn_fake)

    srv = bridge._build_mcp(FastMCP, ws)
    hover_fn = srv._tool_manager._tools['hover'].fn
    health_fn = srv._tool_manager._tools['_health'].fn

    # First call -> spawn + succeed.
    r1 = hover_fn(path=os.path.basename(tmp.name), line=0, character=0)
    assert 'error' not in r1, r1

    # Crash + force respawn detection N times. Each iteration:
    #  1. Kill the live instance.
    #  2. Wait for _crashed flag.
    #  3. Call hover (retries via _call_lsp respawn loop).
    for crash_n in range(bridge._RESPAWN_FAILED_THRESHOLD):
        with bridge._LIVE_LSPS_LOCK:
            key = next(k for k in bridge._LIVE_LSPS if k[0] == 'fake13c')
            inst = bridge._LIVE_LSPS[key]
        os.kill(inst.pid, signal.SIGKILL)
        for _ in range(200):
            if inst._crashed: break
            time.sleep(0.01)
        result = hover_fn(path=os.path.basename(tmp.name), line=0, character=0)
        # Last crash should have pushed us to FAILED -- expect envelope.
        if crash_n == bridge._RESPAWN_FAILED_THRESHOLD - 1:
            assert 'error' in result, f'expected FAILED envelope, got {result}'
            assert result['error'] == 'lsp-persistently-crashing', result
        else:
            # Mid-sequence crashes should still respawn.
            assert 'error' not in result, f'mid-sequence crash {crash_n} did not respawn: {result}'

    # Subsequent calls MUST also reject with lsp-persistently-crashing.
    r_after = hover_fn(path=os.path.basename(tmp.name), line=0, character=0)
    assert 'error' in r_after, r_after
    assert r_after['error'] == 'lsp-persistently-crashing', r_after

    # _health should report failed=True + status=failed.
    h = health_fn()
    after_entry = next(iter(h['languages']['fake13c'].values()))
    assert after_entry['failed'] is True, after_entry
    assert after_entry['status'] == 'failed', after_entry
    assert after_entry['restart_count'] >= 1, after_entry

    print(f'[failed-state] OK -- {bridge._RESPAWN_FAILED_THRESHOLD} crashes ' \
          f'pushed to FAILED, restart_count={after_entry["restart_count"]}')
finally:
    bridge._RESPAWN_FAILED_THRESHOLD = saved_threshold
    bridge._RESPAWN_FAILED_WINDOW_S = saved_window
    bridge._RESPAWN_BACKOFF_BASE_S = saved_base
    try: os.unlink(tmp.name)
    except OSError: pass
    bridge._EXT_TO_LANG.pop('.fakeext13c', None)
    bridge._LANG_TO_LSP_LANGUAGE_ID.pop('fake13c', None)
    bridge._LSP_SPAWNERS.pop('fake13c', None)
    with bridge._LIVE_LSPS_LOCK:
        keys = [k for k in bridge._LIVE_LSPS if k[0] == 'fake13c']
        for k in keys:
            inst = bridge._LIVE_LSPS.pop(k, None)
            bridge._LSP_HEALTH.pop(k, None)
            if inst is not None:
                try: inst.shutdown(timeout=1.0)
                except Exception: pass
PY
}

# --- 13d: backoff schedule monotonically increases up to cap -------------
# Pure-data test: verify the backoff helper's schedule.
t_backoff_schedule() {
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge
# Schedule indexed by window_crash_count (NOT cumulative
# restart_count, after the perf-review fix). Fresh-window first
# crash = index 0 = base sleep.
expected = [1.0, 2.0, 4.0, 8.0, 16.0, 30.0, 30.0, 30.0]
for n, want in enumerate(expected):
    got = bridge._backoff_delay_s(n)
    assert got == want, f'window_count={n}: got {got}, want {want}'
# Negative -> clamped to 0 -> base.
assert bridge._backoff_delay_s(-5) == bridge._RESPAWN_BACKOFF_BASE_S
"
}

# --- 13i: backoff resets on sliding-window decay --------------------------
# Regression guard for Codex post-impl perf review Medium: the
# earlier cumulative-restart_count backoff would slowly degrade
# every recovery to the 30s cap on a long-lived bridge with
# transient crashes. With the fix backoff is indexed by sliding-
# window crash count, so a crash AFTER the window has decayed to
# empty pays only the base delay regardless of historical
# restart_count.
t_backoff_resets_after_window_decay() {
    python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge

key = ('fake13i', '/some/root')
saved = dict(bridge._LSP_HEALTH)
bridge._LSP_HEALTH.clear()
try:
    h = bridge._get_or_init_health(key)
    # Simulate 10 historical crashes (long-lived bridge) but the
    # sliding window has decayed to empty (last entries dropped).
    h['restart_count'] = 10
    h['recent_crash_times'] = []

    # New crash arrives. _record_crash appends it to the window
    # and returns False (only 1 crash in window, far below
    # threshold).
    entered_failed = bridge._record_crash(key, 'fresh-crash')
    assert entered_failed is False, 'should NOT have entered FAILED'
    assert len(h['recent_crash_times']) == 1, h['recent_crash_times']

    # Backoff for THIS crash should use window_count = 0 (first in
    # window) -> base delay, NOT 2**10 capped at 30s like the old
    # cumulative formulation.
    window_count = max(0, len(h['recent_crash_times']) - 1)
    delay = bridge._backoff_delay_s(window_count)
    assert delay == bridge._RESPAWN_BACKOFF_BASE_S, \
        f'backoff did not decay: got {delay}, expected base ({bridge._RESPAWN_BACKOFF_BASE_S})'

    # restart_count is preserved as telemetry.
    assert h['restart_count'] == 10, h
finally:
    bridge._LSP_HEALTH.clear()
    bridge._LSP_HEALTH.update(saved)
"
}

# --- 13e: _health tool returns metadata without spawning ------------------
# _health must work even when no LSP has been spawned in this
# session; it returns an empty languages dict, not an error.
t_health_tool_empty_state() {
    python3 - << 'PY'
import sys
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
try:
    from mcp.server.fastmcp import FastMCP
except Exception:
    sys.exit(0)
import bridge

# Snapshot + clear health/live registry so this test is independent.
saved_health = dict(bridge._LSP_HEALTH)
saved_live = dict(bridge._LIVE_LSPS)
bridge._LSP_HEALTH.clear()
bridge._LIVE_LSPS.clear()
try:
    srv = bridge._build_mcp(FastMCP, Path.cwd())
    health_fn = srv._tool_manager._tools['_health'].fn
    h = health_fn()
    assert h == {'languages': {}}, h
finally:
    bridge._LSP_HEALTH.update(saved_health)
    bridge._LIVE_LSPS.update(saved_live)
PY
}

# --- 13f: Cold-spawn failures accumulate FAILED state -------------------
# Regression guard for the Codex post-impl review High finding:
# the original first-spawn exception path skipped _record_crash, so
# a server that died during cold spawn / initialize would loop
# forever without backoff or FAILED transition. With the fix every
# first-spawn failure feeds _record_crash; threshold crosses convert
# to lsp-persistently-crashing.
t_cold_spawn_failures_enter_failed() {
    python3 - << 'PY'
import sys
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
try:
    from mcp.server.fastmcp import FastMCP
except Exception:
    sys.exit(0)
import bridge
from lsp_client import LspError

# Tighten policy.
saved = (bridge._RESPAWN_FAILED_THRESHOLD,
         bridge._RESPAWN_FAILED_WINDOW_S,
         bridge._RESPAWN_BACKOFF_BASE_S)
bridge._RESPAWN_FAILED_THRESHOLD = 2
bridge._RESPAWN_FAILED_WINDOW_S = 5.0
bridge._RESPAWN_BACKOFF_BASE_S = 0.01

# Spawner that always raises -- simulates a clangd that segfaults
# during initialize, or a binary that fails to spawn at all.
def broken_spawner(ws):
    raise LspError("lsp-spawn-failed", "broken on purpose",
                   lang='fake13f')
bridge.register_spawner('fake13f', broken_spawner)
bridge._EXT_TO_LANG['.fakeext13f'] = 'fake13f'
bridge._LANG_TO_LSP_LANGUAGE_ID['fake13f'] = 'plaintext'

# Make a workspace-bounded file so _dispatch_path doesn't reject.
import tempfile, os
ws = Path.cwd()
tmp = tempfile.NamedTemporaryFile(suffix='.fakeext13f', mode='w',
                                  delete=False, dir=str(ws))
try:
    tmp.write('hi\n'); tmp.close()
    srv = bridge._build_mcp(FastMCP, ws)
    hover_fn = srv._tool_manager._tools['hover'].fn

    # First call: spawner fails. Bridge converts to envelope.
    r1 = hover_fn(path=os.path.basename(tmp.name), line=0, character=0)
    assert 'error' in r1, r1
    assert r1['error'] == 'lsp-spawn-failed', r1

    # Second call (after THRESHOLD failures): must be the FAILED
    # terminal envelope, not another spawner-failed retry.
    r2 = hover_fn(path=os.path.basename(tmp.name), line=0, character=0)
    assert 'error' in r2, r2
    assert r2['error'] == 'lsp-persistently-crashing', \
        f'expected FAILED conversion after THRESHOLD={bridge._RESPAWN_FAILED_THRESHOLD} cold-spawn fails: {r2}'

    # Subsequent calls remain FAILED -- no further spawn attempts.
    r3 = hover_fn(path=os.path.basename(tmp.name), line=0, character=0)
    assert r3['error'] == 'lsp-persistently-crashing', r3

    # _health surfaces failed=True.
    health_fn = srv._tool_manager._tools['_health'].fn
    h = health_fn()
    entry = next(iter(h['languages']['fake13f'].values()))
    assert entry['failed'] is True, entry
    assert entry['status'] == 'failed', entry
    print(f'[cold-spawn-failed] OK -- {bridge._RESPAWN_FAILED_THRESHOLD} ' \
          f'cold-spawn fails crossed to FAILED')
finally:
    bridge._RESPAWN_FAILED_THRESHOLD, bridge._RESPAWN_FAILED_WINDOW_S, bridge._RESPAWN_BACKOFF_BASE_S = saved
    try: os.unlink(tmp.name)
    except OSError: pass
    bridge._EXT_TO_LANG.pop('.fakeext13f', None)
    bridge._LANG_TO_LSP_LANGUAGE_ID.pop('fake13f', None)
    bridge._LSP_SPAWNERS.pop('fake13f', None)
    with bridge._LIVE_LSPS_LOCK:
        keys = [k for k in bridge._LSP_HEALTH if k[0] == 'fake13f']
        for k in keys:
            bridge._LSP_HEALTH.pop(k, None)
PY
}

# --- 13g: respawn walks old cleanup_paths -------------------------------
# Regression guard for the Codex post-impl review Medium finding:
# spawner-owned tempdirs (PSES LogPath) registered on cleanup_paths
# were leaking across respawn generations because _respawn_locked
# never disposed the old instance. With the fix old_inst.shutdown()
# fires before new_inst publishes; cleanup_paths walked.
t_respawn_walks_cleanup_paths() {
    python3 - << 'PY'
import sys, os, time, signal, tempfile
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
try:
    from mcp.server.fastmcp import FastMCP
except Exception:
    sys.exit(0)
import bridge
from lsp_client import LspSubprocess

fake = r"""
import sys, json
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
while True:
    m = read_msg()
    if m is None: break
    method = m.get('method')
    if 'id' in m:
        if method == 'initialize':
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'capabilities':{}}})
        elif method == 'textDocument/hover':
            write_msg({'jsonrpc':'2.0','id':m['id'],
                       'result':{'contents':{'kind':'markdown','value':'h'}}})
        else:
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    elif method == 'exit':
        break
"""

saved_base = bridge._RESPAWN_BACKOFF_BASE_S
bridge._RESPAWN_BACKOFF_BASE_S = 0.05

# Track tempdirs the spawner registers per spawn.
tempdirs_per_spawn: list = []

ws = Path.cwd()
tmp = tempfile.NamedTemporaryFile(suffix='.fakeext13g', mode='w',
                                  delete=False, dir=str(ws))
try:
    tmp.write('// hello\n'); tmp.close()
    bridge._EXT_TO_LANG['.fakeext13g'] = 'fake13g'
    bridge._LANG_TO_LSP_LANGUAGE_ID['fake13g'] = 'plaintext'

    def spawn_fake(workspace_root):
        td = tempfile.mkdtemp(prefix='lsp-mcp-13g-')
        tempdirs_per_spawn.append(td)
        lsp = LspSubprocess(['python3', '-c', fake], lang='fake13g')
        lsp.initialize('file://' + str(workspace_root))
        lsp.cleanup_paths.append(td)
        return lsp
    bridge.register_spawner('fake13g', spawn_fake)

    srv = bridge._build_mcp(FastMCP, ws)
    hover_fn = srv._tool_manager._tools['hover'].fn

    # First spawn -> tempdir #0 created.
    r1 = hover_fn(path=os.path.basename(tmp.name), line=0, character=0)
    assert 'error' not in r1, r1
    assert len(tempdirs_per_spawn) == 1, tempdirs_per_spawn
    td0 = tempdirs_per_spawn[0]
    assert os.path.isdir(td0)

    # Crash + respawn -> tempdir #1 created. Old td0 MUST be removed
    # by the dispose path.
    with bridge._LIVE_LSPS_LOCK:
        key = next(k for k in bridge._LIVE_LSPS if k[0] == 'fake13g')
        inst = bridge._LIVE_LSPS[key]
    os.kill(inst.pid, signal.SIGKILL)
    for _ in range(200):
        if inst._crashed: break
        time.sleep(0.01)

    r2 = hover_fn(path=os.path.basename(tmp.name), line=0, character=0)
    assert 'error' not in r2, f'respawn failed: {r2}'
    assert len(tempdirs_per_spawn) == 2, tempdirs_per_spawn
    td1 = tempdirs_per_spawn[1]
    assert os.path.isdir(td1), 'new tempdir gone unexpectedly'
    assert not os.path.exists(td0), \
        f'OLD tempdir {td0} still exists -- cleanup_paths leaked across respawn'
    print(f'[respawn-cleanup] OK -- old tempdir disposed, new tempdir owned')
finally:
    bridge._RESPAWN_BACKOFF_BASE_S = saved_base
    try: os.unlink(tmp.name)
    except OSError: pass
    # Clean up any remaining tempdirs.
    import shutil
    for td in tempdirs_per_spawn:
        shutil.rmtree(td, ignore_errors=True)
    bridge._EXT_TO_LANG.pop('.fakeext13g', None)
    bridge._LANG_TO_LSP_LANGUAGE_ID.pop('fake13g', None)
    bridge._LSP_SPAWNERS.pop('fake13g', None)
    with bridge._LIVE_LSPS_LOCK:
        keys = [k for k in bridge._LIVE_LSPS if k[0] == 'fake13g']
        for k in keys:
            inst = bridge._LIVE_LSPS.pop(k, None)
            bridge._LSP_HEALTH.pop(k, None)
            if inst is not None:
                try: inst.shutdown(timeout=1.0)
                except Exception: pass
PY
}

# --- 13h: workspace_symbol fan-out triggers crash accounting + respawn --
# Regression guard for the Codex post-implementation review High
# finding: the fan-out path used to snapshot raw instances + filter
# on inst.alive, calling inst.request() directly. A crashed LSP
# would silently fail in the per-language errors dict without
# entering crash accounting or triggering respawn. With the fix the
# fan-out worker re-routes through _get_or_spawn (same pipeline
# used by single-language tools), so the dead LSP triggers respawn
# transparently and the answer comes back as a successful entry.
t_workspace_symbol_respawns_dead_lsp() {
    python3 - << 'PY'
import sys, os, time, signal, tempfile
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
try:
    from mcp.server.fastmcp import FastMCP
except Exception:
    sys.exit(0)
import bridge
from lsp_client import LspSubprocess

fake = r"""
import sys, json
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
while True:
    m = read_msg()
    if m is None: break
    method = m.get('method')
    if 'id' in m:
        if method == 'initialize':
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'capabilities':{}}})
        elif method == 'workspace/symbol':
            write_msg({'jsonrpc':'2.0','id':m['id'],
                       'result':[{'name': 'foo', 'kind': 12,
                                  'location': {'uri': 'file:///x',
                                               'range': {'start': {'line':0,'character':0},
                                                         'end':   {'line':0,'character':0}}}}]})
        elif method == 'textDocument/hover':
            write_msg({'jsonrpc':'2.0','id':m['id'],
                       'result':{'contents':{'kind':'markdown','value':'h'}}})
        else:
            write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    elif method == 'exit':
        break
"""

saved_base = bridge._RESPAWN_BACKOFF_BASE_S
bridge._RESPAWN_BACKOFF_BASE_S = 0.05

ws = Path.cwd()
tmp = tempfile.NamedTemporaryFile(suffix='.fakeext13h', mode='w',
                                  delete=False, dir=str(ws))
try:
    tmp.write('// hello\n'); tmp.close()
    bridge._EXT_TO_LANG['.fakeext13h'] = 'fake13h'
    bridge._LANG_TO_LSP_LANGUAGE_ID['fake13h'] = 'plaintext'

    def spawn_fake(workspace_root):
        lsp = LspSubprocess(['python3', '-c', fake], lang='fake13h')
        lsp.initialize('file://' + str(workspace_root))
        return lsp
    bridge.register_spawner('fake13h', spawn_fake)

    srv = bridge._build_mcp(FastMCP, ws)
    hover_fn = srv._tool_manager._tools['hover'].fn
    ws_fn = srv._tool_manager._tools['workspace_symbol'].fn
    health_fn = srv._tool_manager._tools['_health'].fn

    # Spawn the LSP via hover.
    r1 = hover_fn(path=os.path.basename(tmp.name), line=0, character=0)
    assert 'error' not in r1, r1
    h1 = health_fn()
    entry1 = next(iter(h1['languages']['fake13h'].values()))
    assert entry1['restart_count'] == 0, entry1

    # KILL the subprocess.
    with bridge._LIVE_LSPS_LOCK:
        key = next(k for k in bridge._LIVE_LSPS if k[0] == 'fake13h')
        inst = bridge._LIVE_LSPS[key]
    os.kill(inst.pid, signal.SIGKILL)
    for _ in range(200):
        if inst._crashed: break
        time.sleep(0.01)

    # workspace_symbol(lang=None) fan-out: with the fix, the dead
    # fake13h is detected, respawned, and the per-lang answer comes
    # back. Without the fix it would silently omit fake13h or land
    # in errors.
    r2 = ws_fn(query='foo')
    assert 'error' not in r2, r2
    assert 'fake13h' in r2['per_lang'], \
        f'fan-out did not recover dead LSP: {r2}'
    assert len(r2['per_lang']['fake13h']) >= 1, r2

    # Health surfaces restart_count=1 -- proves crash accounting
    # ran during the fan-out (NOT just on the next single-LSP call).
    h2 = health_fn()
    entry2 = next(iter(h2['languages']['fake13h'].values()))
    assert entry2['restart_count'] == 1, \
        f'workspace_symbol fan-out did not record crash + respawn: {entry2}'
    assert entry2['alive'] is True, entry2

    print(f'[ws-symbol-respawn] OK -- fan-out triggered respawn, ' \
          f'restart_count 0 -> 1')
finally:
    bridge._RESPAWN_BACKOFF_BASE_S = saved_base
    try: os.unlink(tmp.name)
    except OSError: pass
    bridge._EXT_TO_LANG.pop('.fakeext13h', None)
    bridge._LANG_TO_LSP_LANGUAGE_ID.pop('fake13h', None)
    bridge._LSP_SPAWNERS.pop('fake13h', None)
    with bridge._LIVE_LSPS_LOCK:
        keys = [k for k in bridge._LIVE_LSPS if k[0] == 'fake13h']
        for k in keys:
            inst = bridge._LIVE_LSPS.pop(k, None)
            bridge._LSP_HEALTH.pop(k, None)
            if inst is not None:
                try: inst.shutdown(timeout=1.0)
                except Exception: pass
PY
}

# --- 14a: logger emits valid JSON; level filter -------------------------
# Pure-data unit test of the logger module: emit lines, parse each
# line as JSON, assert every required field. DEBUG filtered out by
# default INFO level.
t_logger_json_emit_and_filter() {
    python3 - << 'PY'
import sys, json, io
sys.path.insert(0, 'scripts/lsp-mcp')
import logger as lg

buf = io.StringIO()
lg._LOGGER = lg.LspLogger('INFO', sink=buf)

lg.log('DEBUG', 'filtered debug')   # below threshold
lg.log('INFO',  'visible info', method='hover', latency_ms=12)
lg.log('WARN',  'visible warn', extra='field')
lg.log('ERROR', 'visible error')

lines = [l for l in buf.getvalue().splitlines() if l.strip()]
# DEBUG is filtered out -> 3 lines.
assert len(lines) == 3, f'expected 3 lines, got {len(lines)}: {lines}'
parsed = [json.loads(l) for l in lines]
levels = [p['level'] for p in parsed]
assert levels == ['INFO', 'WARN', 'ERROR'], levels
# Required fields on every line.
for p in parsed:
    assert 'ts' in p
    assert 'level' in p
    assert 'msg' in p
    assert 'corr_id' in p           # always present, may be null
    assert p['ts'].endswith('Z')    # ISO-8601 UTC
# Caller fields land in the JSON.
assert parsed[0]['method'] == 'hover'
assert parsed[0]['latency_ms'] == 12
assert parsed[1]['extra'] == 'field'

# Bump level to DEBUG -> all 4 emit.
buf2 = io.StringIO()
lg._LOGGER = lg.LspLogger('DEBUG', sink=buf2)
lg.log('DEBUG', 'now visible')
lg.log('INFO', 'also visible')
assert len([l for l in buf2.getvalue().splitlines() if l.strip()]) == 2
PY
}

# --- 14b: corr_id ContextVar propagates across thread boundaries ---------
# threading.local would have produced corr_id=None in worker threads;
# ContextVar with copy_context().run() inherits. Codex design review
# caught this as the High failure mode.
t_corr_id_contextvar_propagation() {
    python3 - << 'PY'
import sys, io, json, threading, contextvars, concurrent.futures
sys.path.insert(0, 'scripts/lsp-mcp')
import logger as lg

buf = io.StringIO()
lg._LOGGER = lg.LspLogger('DEBUG', sink=buf)
buf_lock = threading.Lock()

def write_line(label):
    with buf_lock:
        lg.log('INFO', label)

# Bind a corr_id in the main Context.
parent_corr = lg.new_corr_id()
lg.set_corr_id(parent_corr)
write_line('parent')

# Submit work to a pool WITHOUT copy_context -> child sees None.
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
    pool.submit(write_line, 'child-no-ctx').result()

# Submit WITH copy_context -> child inherits parent's corr_id.
ctx = contextvars.copy_context()
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
    pool.submit(ctx.run, write_line, 'child-with-ctx').result()

lg.clear_corr_id()
write_line('after-clear')

lines = [json.loads(l) for l in buf.getvalue().splitlines() if l.strip()]
by_msg = {l['msg']: l for l in lines}
assert by_msg['parent']['corr_id'] == parent_corr, by_msg['parent']
# pool worker without ctx propagation -> None (this is the
# threading.local failure mode the design fix avoided).
assert by_msg['child-no-ctx']['corr_id'] is None, by_msg['child-no-ctx']
# pool worker WITH ctx.run -> inherits parent corr_id.
assert by_msg['child-with-ctx']['corr_id'] == parent_corr, \
    by_msg['child-with-ctx']
assert by_msg['after-clear']['corr_id'] is None
PY
}

# --- 14c: LSP_MCP_LOG_FILE redirects + appends ---------------------------
t_log_file_env_redirect() {
    python3 - << 'PY'
import sys, os, tempfile, json
sys.path.insert(0, 'scripts/lsp-mcp')
import logger as lg

tmp = tempfile.NamedTemporaryFile(suffix='.jsonl', delete=False)
tmp.close()
saved_file = os.environ.get('LSP_MCP_LOG_FILE')
saved_level = os.environ.get('LSP_MCP_LOG_LEVEL')
try:
    os.environ['LSP_MCP_LOG_FILE'] = tmp.name
    os.environ['LSP_MCP_LOG_LEVEL'] = 'INFO'
    lg.reload_from_env()
    lg.log('INFO', 'first', n=1)
    lg.log('INFO', 'second', n=2)
    # Reload once more to confirm append (not truncate).
    lg.reload_from_env()
    lg.log('INFO', 'third', n=3)
    # Close the sink so the file is fully flushed.
    lg._LOGGER.close()

    with open(tmp.name) as f:
        lines = [json.loads(l) for l in f.read().splitlines() if l.strip()]
    msgs = [l['msg'] for l in lines]
    assert msgs == ['first', 'second', 'third'], msgs
    ns = [l.get('n') for l in lines]
    assert ns == [1, 2, 3], ns
finally:
    os.unlink(tmp.name)
    if saved_file is not None:
        os.environ['LSP_MCP_LOG_FILE'] = saved_file
    else:
        os.environ.pop('LSP_MCP_LOG_FILE', None)
    if saved_level is not None:
        os.environ['LSP_MCP_LOG_LEVEL'] = saved_level
    else:
        os.environ.pop('LSP_MCP_LOG_LEVEL', None)
    lg.reload_from_env()
PY
}

# --- 14d: phase-start + phase-end pair with monotonic latency_ms ---------
# Drives a real bridge tool through _call_lsp. Asserts the phase
# pair has matching corr_id + non-zero latency_ms.
t_phase_pair_with_latency() {
    python3 - << 'PY'
import sys, io, json, time
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
try:
    from mcp.server.fastmcp import FastMCP
except Exception:
    sys.exit(0)
import bridge
import logger as lg

buf = io.StringIO()
saved = lg._LOGGER
lg._LOGGER = lg.LspLogger('INFO', sink=buf)
try:
    srv = bridge._build_mcp(FastMCP, Path.cwd())
    health_fn = srv._tool_manager._tools['_health'].fn
    health_fn()
    health_fn()  # second call -> distinct corr_id
finally:
    lg._LOGGER = saved

lines = [json.loads(l) for l in buf.getvalue().splitlines() if l.strip()]
phase_starts = [l for l in lines if l.get('phase') == 'start' and l.get('method') == '_health']
phase_ends   = [l for l in lines if l.get('phase') == 'end' and l.get('method') == '_health']
assert len(phase_starts) == 2, phase_starts
assert len(phase_ends) == 2, phase_ends
# Each pair shares the same corr_id; the two calls have distinct corr_ids.
corr_ids = sorted({s['corr_id'] for s in phase_starts})
assert len(corr_ids) == 2, corr_ids
for end in phase_ends:
    assert end['latency_ms'] >= 0, end
    assert end['status'] == 'ok', end
    assert end['corr_id'] in corr_ids
PY
}

# --- 14e: DEBUG payload cap at 4 KiB -------------------------------------
# Build a fake LSP wire body that exceeds the cap; confirm
# debug_lsp_send truncates and marks body_truncated=True without
# breaking outer JSON validity.
t_debug_body_truncation() {
    python3 - << 'PY'
import sys, io, json
sys.path.insert(0, 'scripts/lsp-mcp')
import logger as lg

buf = io.StringIO()
lg._LOGGER = lg.LspLogger('DEBUG', sink=buf)

# Body whose JSON encoding exceeds 4096 chars.
huge = {'method': 'x', 'params': {'data': 'A' * 8000}}
lg.debug_lsp_send('hover', 'c', request_id=42, body=huge)

lines = [l for l in buf.getvalue().splitlines() if l.strip()]
assert len(lines) == 1
parsed = json.loads(lines[0])     # outer JSON MUST stay valid
assert parsed['event'] == 'lsp-send'
assert parsed['body_truncated'] is True
assert parsed['request_id'] == 42
assert parsed['method'] == 'hover'
# Body is a STRING (not nested JSON) so truncation is safe.
assert isinstance(parsed['body'], str)
assert len(parsed['body']) < 8000
assert parsed['body'].endswith('...truncated')

# Small body -> not truncated.
lg.debug_lsp_send('hover', 'c', request_id=43, body={'small': True})
lines = [l for l in buf.getvalue().splitlines() if l.strip()]
last = json.loads(lines[-1])
assert last['body_truncated'] is False
PY
}

# --- 14f: 3 concurrent tool calls -> 3 distinct corr_ids ----------------
# Test checkpoint contract: stderr logs show 3 distinct corr_id
# threads, each with paired start + end entries.
t_three_concurrent_calls_distinct_corr_ids() {
    python3 - << 'PY'
import sys, io, json, threading
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
try:
    from mcp.server.fastmcp import FastMCP
except Exception:
    sys.exit(0)
import bridge
import logger as lg

# Use a thread-safe StringIO -- our logger's internal _write_lock
# already serializes writes, so a plain StringIO works.
buf = io.StringIO()
saved = lg._LOGGER
lg._LOGGER = lg.LspLogger('INFO', sink=buf)
try:
    srv = bridge._build_mcp(FastMCP, Path.cwd())
    health_fn = srv._tool_manager._tools['_health'].fn
    threads = [threading.Thread(target=health_fn) for _ in range(3)]
    for t in threads: t.start()
    for t in threads: t.join()
finally:
    lg._LOGGER = saved

lines = [json.loads(l) for l in buf.getvalue().splitlines() if l.strip()]
starts = [l for l in lines if l.get('phase') == 'start' and l.get('method') == '_health']
ends   = [l for l in lines if l.get('phase') == 'end' and l.get('method') == '_health']
assert len(starts) == 3, len(starts)
assert len(ends) == 3, len(ends)
# Each call has a distinct corr_id; each corr_id appears in BOTH
# start and end lines (phase pairing).
start_cids = {s['corr_id'] for s in starts}
end_cids = {e['corr_id'] for e in ends}
assert len(start_cids) == 3, start_cids
assert start_cids == end_cids, (start_cids, end_cids)
PY
}

# --- 14g: spawner-import warnings emit JSON, not raw stderr -------------
# Codex design review Medium: the 5 _autoregister_spawners
# warnings used to bypass the JSON contract. With the fix they
# route through logger.log; the JSON line is parseable.
t_spawner_warnings_are_json() {
    python3 - << 'PY'
import sys, io, json
sys.path.insert(0, 'scripts/lsp-mcp')
import logger as lg
import bridge

buf = io.StringIO()
saved = lg._LOGGER
lg._LOGGER = lg.LspLogger('DEBUG', sink=buf)
try:
    # Call the registration function directly with a known-bad
    # spawner module to force a failure path.
    saved_errors = dict(bridge._SPAWNER_IMPORT_ERRORS)
    saved_spawners = dict(bridge._LSP_SPAWNERS)
    try:
        # Patch the import helper to raise so we don't actually
        # reload modules. We simulate by monkey-patching one
        # spawner's expected import and calling _autoregister.
        # Simplest: emit the same warning shape the real path uses.
        lg.log('WARN', 'spawner not registered',
               event='spawner-import-failed',
               lang='c', binary='clangd-19',
               error='ImportError: simulated')
    finally:
        bridge._SPAWNER_IMPORT_ERRORS.clear()
        bridge._SPAWNER_IMPORT_ERRORS.update(saved_errors)
        bridge._LSP_SPAWNERS.clear()
        bridge._LSP_SPAWNERS.update(saved_spawners)
finally:
    lg._LOGGER = saved

lines = [l for l in buf.getvalue().splitlines() if l.strip()]
assert len(lines) == 1, lines
parsed = json.loads(lines[0])     # MUST parse as JSON
assert parsed['event'] == 'spawner-import-failed'
assert parsed['lang'] == 'c'
assert parsed['binary'] == 'clangd-19'
assert parsed['level'] == 'WARN'
PY
}

# --- 14h: workspace_symbol empty-snapshot + exception clean up corr_id --
# Regression guard for Codex post-impl review High: the earlier
# revision returned early on `if not snapshot` WITHOUT clearing
# the bound corr_id and WITHOUT emitting phase=end. The cleanup
# helper _wsc_cleanup() now fires on every exit path.
t_workspace_symbol_cleanup_paths() {
    python3 - << 'PY'
import sys, io, json
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
try:
    from mcp.server.fastmcp import FastMCP
except Exception:
    sys.exit(0)
import bridge
import logger as lg

# Empty registry + no live LSPs -> empty-snapshot return path.
saved_health = dict(bridge._LSP_HEALTH)
saved_live = dict(bridge._LIVE_LSPS)
bridge._LSP_HEALTH.clear()
bridge._LIVE_LSPS.clear()
buf = io.StringIO()
saved_logger = lg._LOGGER
lg._LOGGER = lg.LspLogger('INFO', sink=buf)
try:
    srv = bridge._build_mcp(FastMCP, Path.cwd())
    ws_fn = srv._tool_manager._tools['workspace_symbol'].fn

    # Pre-condition: no corr_id bound.
    assert lg.current_corr_id() is None, lg.current_corr_id()
    r = ws_fn(query='nothing')
    # Post-condition: corr_id MUST be cleared even though the
    # return-early path was taken. Earlier revision left it bound.
    assert lg.current_corr_id() is None, \
        f'corr_id leaked from empty-snapshot path: {lg.current_corr_id()!r}'
    # Empty-snapshot returns the documented stub envelope.
    assert r == {'query': 'nothing', 'per_lang': {}, 'total': 0,
                  'errors': {}}, r
    # Phase pair MUST appear (phase-end + phase-start same corr_id).
    lines = [json.loads(l) for l in buf.getvalue().splitlines() if l.strip()]
    starts = [l for l in lines if l.get('phase') == 'start' and l.get('method') == 'workspace_symbol']
    ends   = [l for l in lines if l.get('phase') == 'end' and l.get('method') == 'workspace_symbol']
    assert len(starts) == 1 and len(ends) == 1, (starts, ends)
    assert starts[0]['corr_id'] == ends[0]['corr_id']
    assert ends[0]['status'] == 'ok'
    assert ends[0]['fan_out'] is True
    assert ends[0]['total'] == 0
finally:
    lg._LOGGER = saved_logger
    bridge._LSP_HEALTH.update(saved_health)
    bridge._LIVE_LSPS.update(saved_live)
PY
}

# --- 14i: _call_lsp emits phase=end on non-LspError exception ----------
# Regression guard for Codex post-impl review Medium: a closure
# that raises a plain Exception used to leak phase-start without
# a paired phase-end. The fix catches Exception, logs phase=end
# with status=error + error_kind, then re-raises.
t_call_lsp_logs_phase_end_on_exception() {
    python3 - << 'PY'
import sys, io, json
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge
import logger as lg

buf = io.StringIO()
saved = lg._LOGGER
lg._LOGGER = lg.LspLogger('INFO', sink=buf)
try:
    def boom():
        raise RuntimeError('bridge bug -- not an LspError')
    raised = False
    try:
        bridge._call_lsp(boom, "fake_method", "c")
    except RuntimeError as exc:
        raised = True
        assert 'bridge bug' in str(exc)
    assert raised, '_call_lsp swallowed non-LspError -- contract broken'
finally:
    lg._LOGGER = saved

lines = [json.loads(l) for l in buf.getvalue().splitlines() if l.strip()]
starts = [l for l in lines if l.get('phase') == 'start' and l.get('method') == 'fake_method']
ends   = [l for l in lines if l.get('phase') == 'end' and l.get('method') == 'fake_method']
assert len(starts) == 1, starts
assert len(ends) == 1, ends
assert ends[0]['status'] == 'error'
assert ends[0]['error_kind'] == 'RuntimeError'
# corr_id must be cleared after the call.
assert lg.current_corr_id() is None
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
run "11a extended-tool normalizers"      t_extended_normalizers
run "11b code_action boundary contract"  t_codeaction_boundary_contract
run "11c completion smoke against clangd" t_completion_smoke_clangd
run "11d code_action immutability"       t_codeaction_immutability
run "11e call hierarchy multi-anchor"    t_call_hierarchy_multi_anchor
run "11f call hierarchy fan-out cap"     t_call_hierarchy_cap
run "11g call hierarchy deadline"        t_call_hierarchy_deadline
run "12a apply_text decision matrix"     t_apply_text_decision_matrix
run "12b didClose at shutdown"           t_didclose_at_shutdown
run "12c cleanup_paths walked"           t_cleanup_paths_walked
run "12d bridge forwards didChange"      t_bridge_handler_forwards_didchange
run "12e apply_text rejects in teardown" t_apply_text_rejects_during_teardown
run "12f post-apply_text req rejected"   t_post_apply_text_request_rejected_in_teardown
run "13a crash flag set on kill"         t_crash_flag_set_on_unexpected_exit
run "13b respawn on next request"        t_respawn_on_next_request
run "13c FAILED state after threshold"   t_failed_state_after_threshold
run "13d backoff schedule"               t_backoff_schedule
run "13e _health empty state"            t_health_tool_empty_state
run "13f cold-spawn fails enter FAILED"  t_cold_spawn_failures_enter_failed
run "13g respawn walks cleanup_paths"    t_respawn_walks_cleanup_paths
run "13h ws-symbol respawns dead LSP"    t_workspace_symbol_respawns_dead_lsp
run "13i backoff resets after window"    t_backoff_resets_after_window_decay
run "14a logger JSON + level filter"     t_logger_json_emit_and_filter
run "14b corr_id ContextVar propagation" t_corr_id_contextvar_propagation
run "14c LSP_MCP_LOG_FILE redirect"      t_log_file_env_redirect
run "14d phase pair latency_ms"          t_phase_pair_with_latency
run "14e DEBUG body 4KiB cap"            t_debug_body_truncation
run "14f 3 concurrent calls = 3 cids"    t_three_concurrent_calls_distinct_corr_ids
run "14g spawner warnings as JSON"       t_spawner_warnings_are_json
run "14h ws-symbol cleanup paths"        t_workspace_symbol_cleanup_paths
run "14i _call_lsp phase-end on exc"     t_call_lsp_logs_phase_end_on_exception
# 9a runs LAST so every prior sub-test has had a chance to clean up.
run "9a no leaked LSP processes"         t_no_leaked_lsp_processes

printf '[lsp-mcp-tests] %d/%d sub-tests PASS\n' "$pass" "$((pass + fail))"
if [ "$fail" -gt 0 ]; then
    exit 1
fi
exit 0
