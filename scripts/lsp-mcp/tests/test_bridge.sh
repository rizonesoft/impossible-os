#!/usr/bin/env bash
# ============================================================================
# scripts/lsp-mcp/tests/test_bridge.sh -- regression pack for the LSP-MCP
#                                         bridge (TODO-07 in 00-infrastructure).
#
# Sub-test coverage in this revision (bridge skeleton only):
#   1a -- --self-test exits 0 with zero LSPs spawned + the expected banner.
#   1b -- import smoke: both modules import cleanly (no side effects at
#         import time, no mcp-SDK requirement at module scope).
#   1c -- LspSubprocess lifecycle: spawn `cat`, shutdown, pgrep empty.
#   1d -- LSP JSON-RPC round-trip against a fake stdio LSP: initialize,
#         request/response demux under 5 concurrent calls, graceful
#         shutdown + post-shutdown request rejected.
#   1e -- LspError.to_envelope reserves `error` + `detail` against extras.
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
    # Banner shape: "[lsp-mcp] OK: 0 LSPs spawned, bridge ready"
    # OR "[lsp-mcp] SKIP: mcp SDK not installed; ..." (CI without SDK).
    echo "$out" | grep -qE '^\[lsp-mcp\] (OK: 0 LSPs spawned, bridge ready|SKIP: mcp SDK not installed)'
}

# --- 1b: import smoke --------------------------------------------------------
t_import() {
    python3 -c '
import sys
sys.path.insert(0, "scripts/lsp-mcp")
import bridge, lsp_client
assert callable(bridge.register_spawner)
assert bridge._LIVE_LSPS == {}
assert bridge._LSP_SPAWNERS == {}
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

run "1a --self-test banner"              t_selftest
run "1b module import smoke"             t_import
run "1c LspSubprocess lifecycle + pgrep" t_subprocess_lifecycle
run "1d JSON-RPC round-trip + shutdown"  t_jsonrpc_roundtrip
run "1e envelope reserves keys"          t_envelope_reserved
run "1f over-length header rejected"     t_protocol_error_oversized_header
run "1g workspace root via __file__"     t_workspace_root

printf '[lsp-mcp-tests] %d/%d sub-tests PASS\n' "$pass" "$((pass + fail))"
if [ "$fail" -gt 0 ]; then
    exit 1
fi
exit 0
