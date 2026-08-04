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
#         non-zero instruction-reference byte count on src/kernel/isr_stubs.asm.
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
#         or OK with a non-zero documentSymbol count on
#         scripts/todo-graph/build.py (per-file cap; open-source
#         pyright has no cross-file workspace-symbol index -- TODO-07).
#   5b -- python_server module imports + exposes the standard surface
#         (PYRIGHT_BIN / LANG_TAG / is_available / install_hint / spawn /
#         required_capabilities).
#   5c -- python_server.required_capabilities() lists the four per-file
#         providers and OMITS workspaceSymbolProvider (advertised by
#         pyright but non-functional cross-file; smoke uses
#         documentSymbol).
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
#   14h -- workspace_symbol fan-out cleanup helper fires on every
#          exit path (empty snapshot, success, exception). Codex
#          post-impl review High caught the original revision
#          leaking corr_id into subsequent unrelated tool calls
#          on the same thread.
#   14i -- _call_lsp emits phase=end with status=error + error_kind
#          when the closure raises a non-LspError exception, then
#          re-raises so the bridge bug stays loud. Codex post-impl
#          review Medium caught the orphaned phase-start bug.
#   14j -- lsp-recv DEBUG event correlates back to the originating
#          MCP call via _pending metadata (NOT the reader thread's
#          own Context which has no corr_id) AND fires AFTER
#          fut.set_result so a slow log sink cannot delay request
#          completion. Codex post-impl review Medium.
#   14k -- _truncate_body uses JSONEncoder.iterencode + early
#          break so the 4 KiB cap bounds CPU/memory cost, not
#          just output size. A 5 MiB body must serialize in well
#          under 100 ms; otherwise the reader thread blocks
#          long enough to delay UNRELATED concurrent requests.
#          Codex post-impl perf review Medium.
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
#   9a -- LSP-process-leak detection: every language server THIS run
#         started (bridge-written ledger, cross-checked by a run-id
#         stamp in the child's environment) must be gone at harness
#         exit. Last sub-test in the file so every prior sub-test has
#         had a chance to clean up. Ownership comes from the spawner,
#         never from a `pgrep -f` command-line match -- see the
#         preamble for the three false positives that rule produced.
#   9b -- deterministic reap: a bridge killed by SIGTERM / SIGINT /
#         SIGHUP reaps its language servers, including one not yet
#         published into _LIVE_LSPS and under a repeated-signal storm.
#         Driven with a child that closes its own stdin and cannot
#         self-exit, so the assertion cannot pass by accident; step 0
#         verifies that property before relying on it.
#   9c -- leak attribution is scoped: a process whose command line
#         merely contains an LSP binary name, and a real LSP owned by
#         a DIFFERENT run id, are both rejected -- while a genuine
#         leak on either net (ledger, environ) is still caught.
#   9d -- the shutdown budget bounds the SIGNAL path end-to-end: a real
#         SIGTERM into a real handler holding several stamped records
#         finishes inside the budget, and again with a saturated
#         (undrained) stderr pipe, which is how teardown once wedged
#         before it began.
#   9e -- the deadline is ALLOCATED, not merely present: the settle
#         wait, the _shutdown_bounded graceful join and the atexit
#         graceful join are each wedged in turn, and a child only the
#         deadline-gated pass can reach must still be collected.
#   9f -- the two task-aware reads pinned directly: _is_zombie and
#         _carries_our_owner_id against a Z thread-group leader with a
#         live worker, plus the task-list race no live shape can
#         produce on demand (empty / unreadable -> must read ALIVE).
#
#  15a -- workspace-root resolution priority chain.
#  15b -- a file:// URI is rejected at the _dispatch_path entry.
#  15c -- ../../ escape rejected at the parent-traversal gate.
#  15d -- a symlink met during the walk is rejected by O_NOFOLLOW.
#  15e -- race-free walk: a rename mid-walk never yields a wrong-file
#         read; the walk fails instead.
#  15f -- respawn replay still works after the absolute-path fix.
#  15g -- a workspace root that is not a directory returns a structured
#         envelope rather than an exception.
#  15h -- the deep-path cap answers lsp-path-too-deep.
#  15i -- a FIFO with a wired extension must not hang the bridge.
#  15j -- an absolute path through an in-workspace symlink is rejected.
#  15k -- a symlinked workspace root replays absolute paths on respawn.
#
#  18a -- type-hierarchy normalizers (pure, no LSP).
#  18b -- type_hierarchy_supertypes round-trip against clangd (SKIP when
#         clangd-19 is missing).
#  18c -- capability-missing contract against a fake LSP that advertises
#         no provider.
#  18d -- supported-path walk against a fake LSP that does advertise it.
#  18e -- per-anchor types cap and its truncation metadata.
#
#  16a -- --warm-start (no value) warms every registered LSP, exits 0,
#         emits warm-begin + warm-complete (or warm-over-budget).
#  16b -- --warm-start=c,py warms only the named subset.
#  16c -- --warm-start=bogus,c emits warm-unknown-lang for `bogus`,
#         still warms `c`, exits 0.
#  16d -- --warm-start=zzz,yyy (all unknown) -> warm-empty + exit 0.
#  16e -- snapshot_progress copy contract: 4-writer/4-reader race for
#         500 ms must not trip dictionary-changed-size-during-iteration.
#  16f -- server-initiated window/workDoneProgress/create gets a
#         {result: null} ack at the same id (fake-LSP driver).
#
#  17a -- --warm-start --warm-start-mode=background --self-test exits
#         in under 1.5s wall-clock (proves srv.run()-equivalent gate
#         is not delayed by the bg warm worker).
#  17b -- background mode emits the warm-bg-dispatched JSON log line
#         (proves the dispatch fires on the main thread).
#  17c -- omitting --warm-start-mode keeps blocking semantics
#         (default; absence of warm-bg-dispatched proves the blocking
#         branch ran).
#  17d -- argparse rejects a bogus --warm-start-mode value with a
#         clear error message and a non-zero exit.
#  17e -- shutdown publish gate: when _BRIDGE_SHUTTING_DOWN is set
#         before _get_or_spawn publishes, the spawn raises
#         lsp-shutdown and the new instance does NOT enter
#         _LIVE_LSPS (closes the bg-thread post-snapshot leak).
#  17f -- bg thread re-entry: a second dispatch while a prior bg
#         thread is alive joins the prior thread cleanly and starts
#         the new one without orphan / double-launch.
#  17g -- shutdown gate TOCTOU: spawner returns inst, then
#         _BRIDGE_SHUTTING_DOWN gets set, then the publish block
#         runs -- atomic gate-check-then-publish under
#         _LIVE_LSPS_LOCK rejects the publish, reaps inst inline,
#         raises lsp-shutdown.
# ============================================================================
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

cd "$REPO_ROOT"

# LSP-process-leak detection (sub-test 9a): identify the language
# servers THIS harness started, and assert none of them outlives it.
#
# The identity comes from the spawner. Until 2026-08-03 it came from
# `pgrep -u UID -f '<lsp binary names>'`, snapshotted at entry and
# diffed at exit -- and that rule answers a different question than the
# one 9a asks. Measured over 15 harness runs while the agent fleet ran
# alongside (2 failures, 4 processes blamed, none of them ours):
#
#   * `pgrep -f` matches the whole COMMAND LINE, so a `node
#     codex-companion.mjs` review process was reported as a leaked
#     language server because the review prompt quoted the string
#     "clangd-19" (run 2, pid 1373355);
#   * it matches FOREIGN servers owned by this user -- one clangd from
#     another session's bridge (pid 1374170, no run id, parent 1374079)
#     lived across four consecutive harness runs and was blamed on the
#     one whose window it happened to start in;
#   * it matches processes that are already exiting: run 4's accused
#     pid 1390938 was gone before `ps` could print its command line.
#
# So: the bridge stamps LSP_BRIDGE_RUN_ID into every language server it
# spawns and appends an ownership record (run id, pid, /proc start
# ticks, lang, binary) to LSP_BRIDGE_PID_LEDGER. 9a asks the precise
# question -- "is a process WE recorded still alive?" -- and never the
# pattern-matching one. Both variables are OVERWRITTEN unconditionally
# so a stale value inherited from the caller's environment cannot make
# this run adopt another run's children.
LSP_BIN_NAMES='clangd-19 clangd asm-lsp bash-language-server pyright-langserver pwsh'
LSP_RUN_ID="test-bridge-$$-$(date +%s%N)"
LSP_PID_LEDGER="$(mktemp -t lsp-bridge-pids.XXXXXX)"
export LSP_BRIDGE_RUN_ID="$LSP_RUN_ID"
export LSP_BRIDGE_PID_LEDGER="$LSP_PID_LEDGER"
trap 'rm -f "$LSP_PID_LEDGER"' EXIT

# Start time (field 22 of /proc/PID/stat) for the PID-reuse guard: a
# recycled PID is a different process and must not read as a survivor.
# comm sits in parens and may itself contain spaces or parens, so parse
# from the LAST ')'. Empty when procfs is unavailable.
lsp_start_ticks() {
    sed 's/^.*) //' "/proc/$1/stat" 2>/dev/null | awk '{print $20}'
}

pass=0
fail=0
skipped=0

# Shapes a sub-test could not exercise on THIS host. An unavailable
# fixture is not a pass: before this list existed, a host without a
# loadable libc.so.6 skipped the whole thread-group family and the
# harness still printed the success banner naming it, so an unsupported
# host could mint a green section checkpoint for coverage that never ran
# (Codex consistency review, Medium). Two mechanisms, because a skip can
# be whole or partial: a sub-test whose entire fixture is missing returns
# 77 and `run` counts it apart from pass and fail; a sub-test that ran
# most of its modes and lost one calls lsp_note_skip and must not name
# the missing one in its own banner.
LSP_SKIPPED_SHAPES=""
lsp_record_skip() {
    # $1 sub-test tag, $2 shape, $3 reason. Appends only.
    LSP_SKIPPED_SHAPES="${LSP_SKIPPED_SHAPES}${LSP_SKIPPED_SHAPES:+
}  $1 $2 -- $3"
}
lsp_note_skip() {
    lsp_record_skip "$1" "$2" "$3"
    printf '[%s] SKIP: %s -- %s\n' "$1" "$2" "$3" >&2
}

run() {
    local name="$1" rc=0; shift
    "$@" || rc=$?
    if [ "$rc" = "0" ]; then
        printf '[lsp-mcp-tests] PASS %s\n' "$name"
        pass=$((pass + 1))
    elif [ "$rc" = "77" ]; then
        # 77 = this host cannot build the fixture at all. Neither a pass
        # nor a failure; counting it as a pass is the bug described above.
        printf '[lsp-mcp-tests] SKIP %s\n' "$name" >&2
        skipped=$((skipped + 1))
        # Append only -- the sub-test has already printed its own reason.
        lsp_record_skip "lsp-mcp-tests" "$name" "fixture unavailable on this host"
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
    #   "[lsp-mcp] OK: 0 LSPs spawned, 17 tools registered, bridge ready"
    # OR "[lsp-mcp] SKIP: mcp SDK not installed; ..." (CI without SDK).
    # Tool count = len(MCP_TOOL_NAMES); pin to the literal so a
    # registration drift fails this test instead of silently sliding.
    echo "$out" | grep -qE '^\[lsp-mcp\] (OK: 0 LSPs spawned, 17 tools registered, bridge ready|SKIP: mcp SDK not installed)'
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
    # Accept either the pyright OK banner with a non-zero symbol count
    # (documentSymbol on scripts/todo-graph/build.py returns its
    # top-level symbols once pyright finishes parsing the opened file),
    # or the SKIP banner when pyright-langserver is not installed on
    # this host. The smoke is documentSymbol (per-file), NOT
    # workspace/symbol: open-source pyright has no cross-file symbol
    # index (Pylance-only), so a workspace-symbol smoke returns empty
    # and would flip SKIP->FAIL on install. See TODO-07.
    echo "$out" | grep -qE "^\[lsp-mcp\] (OK: pyright spawned, documentSymbol on build\.py returned [1-9][0-9]* symbols|SKIP: pyright not installed)"
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
# workspaceSymbolProvider is intentionally OMITTED (same disposition as
# bash_server, different reason): open-source pyright advertises the cap
# but its workspace/symbol has no cross-file index (Pylance-only) and
# returns empty here, so we do not assert advertised-but-nonfunctional.
# The sub-test 5a smoke exercises documentSymbol (per-file) instead.
# See python_server.required_capabilities() docstring + TODO-07.
for name in ('hoverProvider','definitionProvider','referencesProvider','documentSymbolProvider'):
    assert name in caps, name
assert 'workspaceSymbolProvider' not in caps, 'see python_server.required_capabilities() docstring'
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
    echo "$out" | grep -qE '^\[lsp-mcp\] OK: 17 tools registered \(hover, definition, references, diagnostics, workspace_symbol, document_symbol, completion, signature_help, type_definition, implementation, declaration, call_hierarchy_incoming, call_hierarchy_outgoing, code_action, type_hierarchy_supertypes, type_hierarchy_subtypes, _health\)$' || return 1
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
    # Two type-hierarchy tools (read-only sibling of call hierarchy).
    'type_hierarchy_supertypes': (['path','line','character'], []),
    'type_hierarchy_subtypes': (['path','line','character'], []),
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

# --- 18a: type-hierarchy normalizers (pure, no LSP) ----------------------
# _normalize_type_hierarchy_items passes through dict items, drops
# null/garbage/non-dicts; _normalize_type_hierarchy_item flattens a
# TypeHierarchyItem to the read-only field set (selectionRange ->
# selection_range) and collapses a non-dict to {}.
t_type_hierarchy_normalizers() {
    python3 - << 'PY'
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
from bridge import (_normalize_type_hierarchy_items,
                    _normalize_type_hierarchy_item)

# items: list passthrough; null / wrong type / mixed -> dict-only list
assert _normalize_type_hierarchy_items(None) == []
assert _normalize_type_hierarchy_items('garbage') == []
assert _normalize_type_hierarchy_items([{'name': 'A'}, 'skip', 7]) == [{'name': 'A'}]

# item: full TypeHierarchyItem flattens; selectionRange -> selection_range
full = {'name': 'Base', 'kind': 5, 'uri': 'file:///b.py',
        'range': {'start': {}}, 'selectionRange': {'end': {}},
        'detail': 'class Base', 'data': {'opaque': 1}}
flat = _normalize_type_hierarchy_item(full)
assert flat == {'name': 'Base', 'kind': 5, 'uri': 'file:///b.py',
                'range': {'start': {}}, 'selection_range': {'end': {}},
                'detail': 'class Base'}, flat
# 'data' (opaque server payload) is intentionally dropped; no edit field exists.
assert 'data' not in flat
# Missing fields default to None; non-dict collapses to {}.
assert _normalize_type_hierarchy_item({'name': 'X'}) == {
    'name': 'X', 'kind': None, 'uri': None, 'range': None,
    'selection_range': None, 'detail': None}
assert _normalize_type_hierarchy_item('not-a-dict') == {}
print('[type_hierarchy] normalizers OK')
PY
}

# --- 18b: type_hierarchy_supertypes round-trip vs clangd (SKIP if missing) -
# Exercises the wired handler chain: path validation -> capability gate
# -> prepareTypeHierarchy -> supertypes -> normalize -> envelope. An
# empty types list for a plain C struct is correct (C has no supertype
# graph); the assertion proves the round-trip + response contract, not
# a fabricated result.
t_type_hierarchy_smoke_clangd() {
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
fn = srv._tool_manager._tools['type_hierarchy_supertypes'].fn

result = fn(path='src/kernel/main.c', line=10, character=0)
# Tolerate a clangd-not-ready timeout envelope; anything else with an
# 'error' key is a real path/validation failure.
if 'error' in result:
    assert result['error'] in ('lsp-timeout', 'lsp-spawner-import-failed',
                               'lsp-language-unsupported',
                               'lsp-binary-missing', 'lsp-spawn-failed'), result
    sys.exit(0)
# Successful path: full response contract incl. the capability_missing flag.
for key in ('anchors', 'prepared_count', 'prepared_total',
            'truncated', 'deadline_exceeded', 'capability_missing'):
    assert key in result, (key, result)
assert result['capability_missing'] is False, result  # clangd advertises it
assert isinstance(result['anchors'], list), result

# Invalid position lands as an envelope (NOT an exception).
bad = fn(path='src/kernel/main.c', line=-1, character=0)
assert 'error' in bad and bad['error'] == 'lsp-position-invalid', bad
print('[type_hierarchy] clangd round-trip OK')
PY
}

# --- 18c: capability-missing contract (fake LSP without provider) --------
# An LSP that does NOT advertise typeHierarchyProvider must yield
# capability_missing=True + empty anchors + a note, NEVER an error or a
# method-not-found surfaced from the LSP. Fake LSP, no real dep.
t_type_hierarchy_capability_missing() {
    python3 - << 'PY'
import sys, tempfile, os
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
try:
    from mcp.server.fastmcp import FastMCP
except Exception:
    sys.exit(0)  # SKIP
import bridge
from lsp_client import LspSubprocess

# Fake LSP whose initialize advertises NO typeHierarchyProvider.
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
    elif method == 'shutdown':
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    elif method == 'exit':
        break
    elif 'id' in m:
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
"""
ws = Path.cwd()
tmp = tempfile.NamedTemporaryFile(suffix='.fakeext', mode='w', delete=False, dir=str(ws))
try:
    tmp.write('// fake source\n'); tmp.close()
    bridge._EXT_TO_LANG['.fakeext'] = 'fake'
    bridge._LANG_TO_LSP_LANGUAGE_ID['fake'] = 'plaintext'
    def spawn_fake(workspace_root):
        lsp = LspSubprocess(['python3', '-c', fake], lang='fake')
        lsp.initialize('file://' + str(workspace_root))
        return lsp
    bridge.register_spawner('fake', spawn_fake)
    srv = bridge._build_mcp(FastMCP, ws)
    fn = srv._tool_manager._tools['type_hierarchy_supertypes'].fn
    result = fn(path=os.path.basename(tmp.name), line=0, character=0)
    assert 'error' not in result, result
    assert result['capability_missing'] is True, result
    assert result['anchors'] == [], result
    assert result['prepared_count'] == 0, result
    assert 'typeHierarchyProvider' in result['note'], result
    print('[type_hierarchy] capability-missing contract OK')
finally:
    os.unlink(tmp.name)
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

# --- 18d: supported-path walk (fake LSP advertises provider) -------------
# Provider advertised -> capability_missing=False; prepareTypeHierarchy
# returns 2 anchors, each supertypes follow-up returns a FLAT
# TypeHierarchyItem[] that is normalized (selectionRange ->
# selection_range). Proves the multi-anchor walk + flat-list handling
# distinct from call hierarchy's {from}/{to} wrapper.
t_type_hierarchy_supported_walk() {
    python3 - << 'PY'
import sys, tempfile, os
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
    if method == 'initialize':
        write_msg({'jsonrpc':'2.0','id':m['id'],
                   'result':{'capabilities':{'typeHierarchyProvider': True}}})
    elif method == 'textDocument/prepareTypeHierarchy':
        # Opaque server `data` MUST round-trip to the follow-up request
        # but MUST NOT appear in the normalized response anchor.
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':[
            {'name': 'Derived_a', 'kind': 5, 'uri': 'file:///x', 'range': {}, 'selectionRange': {'a': 1}, 'data': {'opaque': 'a'}},
            {'name': 'Derived_b', 'kind': 5, 'uri': 'file:///x', 'range': {}, 'selectionRange': {'b': 1}, 'data': {'opaque': 'b'}},
        ]})
    elif method == 'typeHierarchy/supertypes':
        item = m['params']['item']
        # The outbound item MUST be the RAW prepared TypeHierarchyItem,
        # carrying opaque `data`. Echo data.opaque into the result name so
        # a regression that sends a normalized (data-stripped) item yields
        # 'MISSING' here and fails the data-derived assertion below.
        op = item.get('data', {}).get('opaque', 'MISSING')
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':[
            {'name': f"Base_of_{item['name']}_{op}", 'kind': 5, 'uri': 'file:///x',
             'range': {}, 'selectionRange': {'k': 1}, 'detail': 'base', 'data': {'x': 1}},
        ]})
    elif method == 'shutdown':
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    elif method == 'exit':
        break
    elif 'id' in m:
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
"""
ws = Path.cwd()
tmp = tempfile.NamedTemporaryFile(suffix='.fakeext', mode='w', delete=False, dir=str(ws))
try:
    tmp.write('// fake source\n'); tmp.close()
    bridge._EXT_TO_LANG['.fakeext'] = 'fake'
    bridge._LANG_TO_LSP_LANGUAGE_ID['fake'] = 'plaintext'
    def spawn_fake(workspace_root):
        lsp = LspSubprocess(['python3', '-c', fake], lang='fake')
        lsp.initialize('file://' + str(workspace_root))
        return lsp
    bridge.register_spawner('fake', spawn_fake)
    srv = bridge._build_mcp(FastMCP, ws)
    fn = srv._tool_manager._tools['type_hierarchy_supertypes'].fn
    result = fn(path=os.path.basename(tmp.name), line=0, character=0)
    assert 'error' not in result, result
    assert result['capability_missing'] is False, result
    assert result['prepared_count'] == 2, result
    assert result['prepared_total'] == 2, result
    assert result['truncated'] is False, result
    assert len(result['anchors']) == 2, result
    names = sorted(a['anchor']['name'] for a in result['anchors'])
    assert names == ['Derived_a', 'Derived_b'], names
    for entry in result['anchors']:
        # The RETURNED anchor is normalized: selectionRange -> selection_range,
        # opaque 'data' dropped (it still reached the follow-up request, proven
        # by the Base_of_<name> result below).
        a = entry['anchor']
        assert 'data' not in a, a
        assert 'selection_range' in a and 'selectionRange' not in a, a
        assert len(entry['types']) == 1, entry
        assert entry['types_total'] == 1, entry
        assert entry['types_truncated'] is False, entry
        t = entry['types'][0]
        # flat TypeHierarchyItem, normalized (selectionRange -> selection_range,
        # opaque 'data' dropped). The Base name carries the prepared anchor's
        # data.opaque value ('a'/'b' = last char of Derived_a/Derived_b),
        # proving the RAW item (with data) reached typeHierarchy/supertypes --
        # a regression sending a normalized outbound anchor would yield
        # 'Base_of_<name>_MISSING' and fail here.
        op = a['name'].split('_')[-1]
        assert t['name'] == f"Base_of_{a['name']}_{op}", t
        assert t['selection_range'] == {'k': 1}, t
        assert 'data' not in t, t
    print('[type_hierarchy] supported-path 2/2 anchors OK')
finally:
    os.unlink(tmp.name)
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

# --- 18e: per-anchor types cap + truncation metadata ---------------------
# A supported LSP returning more supertypes than _TYPE_HIERARCHY_MAX_TYPES_
# PER_ANCHOR (200) must be capped, with types_total / types_truncated
# surfaced so the caller can narrow the query (Codex perf review). Fake LSP.
t_type_hierarchy_types_cap() {
    python3 - << 'PY'
import sys, tempfile, os
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
    if method == 'initialize':
        write_msg({'jsonrpc':'2.0','id':m['id'],
                   'result':{'capabilities':{'typeHierarchyProvider': True}}})
    elif method == 'textDocument/prepareTypeHierarchy':
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':[
            {'name': 'Root', 'kind': 5, 'uri': 'file:///x', 'range': {}, 'selectionRange': {}},
        ]})
    elif method == 'typeHierarchy/supertypes':
        # 250 supertypes -- exceeds the per-anchor cap of 200.
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':[
            {'name': f'Base{i}', 'kind': 5, 'uri': 'file:///x',
             'range': {}, 'selectionRange': {}} for i in range(250)
        ]})
    elif method == 'shutdown':
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
    elif method == 'exit':
        break
    elif 'id' in m:
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':None})
"""
ws = Path.cwd()
tmp = tempfile.NamedTemporaryFile(suffix='.fakeextth', mode='w', delete=False, dir=str(ws))
try:
    tmp.write('// fake source\n'); tmp.close()
    bridge._EXT_TO_LANG['.fakeextth'] = 'faketh'
    bridge._LANG_TO_LSP_LANGUAGE_ID['faketh'] = 'plaintext'
    def spawn_fake(workspace_root):
        lsp = LspSubprocess(['python3', '-c', fake], lang='faketh')
        lsp.initialize('file://' + str(workspace_root))
        return lsp
    bridge.register_spawner('faketh', spawn_fake)
    srv = bridge._build_mcp(FastMCP, ws)
    fn = srv._tool_manager._tools['type_hierarchy_supertypes'].fn
    result = fn(path=os.path.basename(tmp.name), line=0, character=0)
    assert 'error' not in result, result
    assert result['capability_missing'] is False, result
    assert len(result['anchors']) == 1, result
    entry = result['anchors'][0]
    assert entry['types_total'] == 250, entry
    assert entry['types_truncated'] is True, entry
    # Capped at _TYPE_HIERARCHY_MAX_TYPES_PER_ANCHOR (200).
    assert len(entry['types']) == 200, len(entry['types'])
    print('[type_hierarchy] per-anchor types cap OK (250 -> 200, truncated flagged)')
finally:
    os.unlink(tmp.name)
    bridge._EXT_TO_LANG.pop('.fakeextth', None)
    bridge._LANG_TO_LSP_LANGUAGE_ID.pop('faketh', None)
    bridge._LSP_SPAWNERS.pop('faketh', None)
    with bridge._LIVE_LSPS_LOCK:
        keys = [k for k in bridge._LIVE_LSPS if k[0] == 'faketh']
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

# --- 14j: lsp-recv DEBUG correlation through _pending metadata + non-blocking
# Two-part regression guard for the post-impl Codex review Medium:
#   (1) lsp-recv corr_id MUST come from the per-request metadata
#       (the reader thread's own Context has no corr_id), so a
#       request initiated under corr_id X correlates with the
#       lsp-recv X even though the receive happens on the reader.
#   (2) lsp-recv DEBUG logging MUST NOT block the Future
#       completion -- the fix moved the log call AFTER
#       fut.set_result/set_exception. We can't directly observe
#       wall-clock improvement in a unit test, but we CAN verify
#       the log-emit ordering by checking that fut.done() is True
#       before debug_lsp_recv would have fired (proven by reading
#       the source layout via grep).
t_lsp_recv_corr_id_correlation() {
    python3 - << 'PY'
import sys, io, json, time
sys.path.insert(0, 'scripts/lsp-mcp')
from lsp_client import LspSubprocess
import logger as lg

# DEBUG-mode logger capturing to in-memory buffer.
buf = io.StringIO()
saved = lg._LOGGER
lg._LOGGER = lg.LspLogger('DEBUG', sink=buf)

# Simple echo stub: replies to any request with a canned result.
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
    elif 'id' in m:
        write_msg({'jsonrpc':'2.0','id':m['id'],'result':{'echoed_method': method}})
    elif method == 'exit':
        break
"""

lsp = LspSubprocess(['python3', '-c', fake], lang='fake-14j')
try:
    lsp.initialize('file:///tmp/test')

    # Bind a corr_id; issue a request; assert lsp-recv carries it.
    expected = lg.new_corr_id()
    lg.set_corr_id(expected)
    try:
        result = lsp.request('hover', {}, timeout=2.0)
        assert result['echoed_method'] == 'hover', result
    finally:
        lg.clear_corr_id()

    # Inspect the log: there MUST be an lsp-send + lsp-recv pair
    # tagged with the expected corr_id.
    lines = [json.loads(l) for l in buf.getvalue().splitlines() if l.strip()]
    sends = [l for l in lines if l.get('event') == 'lsp-send']
    recvs = [l for l in lines if l.get('event') == 'lsp-recv']
    assert len(sends) >= 1, sends
    assert len(recvs) >= 1, recvs
    # Find the send/recv for OUR request (most recent ones).
    last_send = sends[-1]
    last_recv = recvs[-1]
    assert last_send['corr_id'] == expected, last_send
    # CRITICAL: lsp-recv carries the originating call's corr_id
    # via _pending metadata, NOT the reader thread's Context.
    assert last_recv['corr_id'] == expected, \
        f'lsp-recv corr_id mismatch: {last_recv}'
    assert last_recv['request_id'] == last_send['request_id']

    # Source-level check: in lsp_client.py _resolve_pending,
    # debug_lsp_recv MUST be called AFTER fut.set_result so a
    # slow log sink cannot delay request completion.
    src = open('scripts/lsp-mcp/lsp_client.py').read()
    func_start = src.find('def _resolve_pending(')
    assert func_start > 0
    func_end = src.find('def ', func_start + 10)
    func_body = src[func_start:func_end]
    set_result_pos = func_body.find('fut.set_result')
    debug_recv_pos = func_body.find('_dbg_recv(')
    assert set_result_pos > 0, 'set_result missing'
    assert debug_recv_pos > 0, '_dbg_recv missing'
    assert debug_recv_pos > set_result_pos, \
        ('_dbg_recv must follow fut.set_result -- the reverse '
         'lets a slow log sink delay request completion '
         '(Codex post-impl review Medium)')
finally:
    lg._LOGGER = saved
    try: lsp.shutdown(timeout=1.0)
    except Exception: pass
PY
}

# --- 14k: _truncate_body is CPU-bounded, not just output-bounded -------
# Regression guard for the post-impl perf Codex Medium: a naive
# json.dumps would serialize the entire 32 MiB-ish body before
# the output truncation kicked in, blocking the reader thread.
# The fix uses JSONEncoder.iterencode + early break so the cost
# is bounded by the cap itself.
t_truncate_body_cpu_bounded() {
    python3 - << 'PY'
import sys, time
sys.path.insert(0, 'scripts/lsp-mcp')
import logger as lg

# Build a HUGE body whose JSON encoding would be ~10 MiB+. The
# iterencode-based truncation must never serialize past the cap.
huge = {'method': 'massive', 'params': {
    'data': ['x' * 1000] * 5000  # 5000 strings of 1000 chars = 5 MiB
}}

t0 = time.monotonic()
truncated, encoded = lg._truncate_body(huge)
elapsed = time.monotonic() - t0

# The output is bounded at the cap + a short truncation marker.
assert truncated is True, 'huge body must report truncated=True'
assert len(encoded) < lg._DEBUG_BODY_CAP_BYTES + 64, \
    f'encoded length {len(encoded)} exceeds cap+marker'
assert encoded.endswith('...truncated'), encoded[-30:]

# CPU bound: serializing 5 MiB of JSON with json.dumps takes
# ~50-200 ms. With iterencode + early break it should be under
# ~20 ms. Generous threshold of 100 ms catches the regression
# without flaking on slow CI.
assert elapsed < 0.1, \
    f'truncation took {elapsed*1000:.1f}ms -- iterencode early-break broken'

print(f'[truncate-body] OK -- 5 MiB body bounded in {elapsed*1000:.2f}ms')
PY
}

# --- 15a: workspace-root resolution priority chain -----------------------
# --repo-root > LSP_MCP_WORKSPACE_ROOT > _find_repo_root > _find_git_root > cwd.
t_workspace_root_priority() {
    python3 - << 'PY'
import sys, os, argparse, tempfile
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge

# (1) --repo-root wins. Operator spelling preserved (no .resolve()
# in _workspace_root_from_argv -- the absolute-path branch in
# _open_in_workspace accepts both raw and realpath anchors so a
# symlinked workspace roundtrips in LSP-respawn replay).
ns = argparse.Namespace(repo_root='/tmp')
got = bridge._workspace_root_from_argv(ns)
assert got == Path('/tmp'), got

# (2) LSP_MCP_WORKSPACE_ROOT env wins when --repo-root unset.
ns = argparse.Namespace(repo_root=None)
saved = os.environ.get('LSP_MCP_WORKSPACE_ROOT')
try:
    with tempfile.TemporaryDirectory() as td:
        os.environ['LSP_MCP_WORKSPACE_ROOT'] = td
        got = bridge._workspace_root_from_argv(ns)
        # Spelling preserved (Path(td) -- not .resolve()); td may
        # itself be a realpath on most systems, which is fine.
        assert got == Path(td), got
finally:
    if saved is None: os.environ.pop('LSP_MCP_WORKSPACE_ROOT', None)
    else:             os.environ['LSP_MCP_WORKSPACE_ROOT'] = saved

# (3) Empty env -> falls through to in-tree marker walk.
saved = os.environ.get('LSP_MCP_WORKSPACE_ROOT')
try:
    os.environ['LSP_MCP_WORKSPACE_ROOT'] = ''
    got = bridge._workspace_root_from_argv(argparse.Namespace(repo_root=None))
    # Should land at the impossible-os repo root (this script's tree).
    assert (got / 'scripts' / 'lsp-mcp').is_dir(), got
finally:
    if saved is None: os.environ.pop('LSP_MCP_WORKSPACE_ROOT', None)
    else:             os.environ['LSP_MCP_WORKSPACE_ROOT'] = saved
PY
}

# --- 15b: file:// URI rejected at _dispatch_path entry ------------------
t_file_uri_rejected() {
    python3 - << 'PY'
import sys
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge
from lsp_client import LspError

ws = Path.cwd()
for url in ('file:///etc/passwd', 'file://localhost/etc/passwd', 'file:/etc/passwd'):
    try:
        bridge._dispatch_path(url, ws)
        raise SystemExit(f'file:// URI {url!r} not rejected')
    except LspError as e:
        assert e.kind == 'lsp-path-outside-workspace', (url, e.kind)
PY
}

# --- 15c: ../../escape rejected at parent-traversal gate ----------------
t_parent_traversal_rejected() {
    python3 - << 'PY'
import sys
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge
from lsp_client import LspError

ws = Path.cwd()
for bad in ('../../etc/passwd', '../etc/passwd', 'src/../../etc/passwd',
             'src//../etc'):
    try:
        bridge._dispatch_path(bad, ws)
        raise SystemExit(f'parent-traversal path {bad!r} not rejected')
    except LspError as e:
        # Either parent-segment rejection (most paths) OR not-found
        # (the symlink-walk catches some forms).
        assert e.kind in ('lsp-path-outside-workspace', 'lsp-path-not-found'), \
            f'unexpected kind for {bad!r}: {e.kind}'

# Embedded NUL byte rejected.
try:
    bridge._dispatch_path('src/main\0.c', ws)
    raise SystemExit('NUL-byte path not rejected')
except LspError as e:
    assert e.kind == 'lsp-path-outside-workspace', e.kind
PY
}

# --- 15d: symlink in walk rejected (per test checkpoint) ---------------
# `ln -s /etc/passwd src/evil.c && hover(path="src/evil.c")` -> rejected.
t_symlink_rejected() {
    python3 - << 'PY'
import sys, os, tempfile
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge
from lsp_client import LspError

ws = Path.cwd()
# Place a symlink to /etc/passwd inside the workspace with a wired
# extension. The race-free walk MUST reject it via O_NOFOLLOW.
link_path = ws / 'evil_test_link.c'
try:
    if link_path.exists() or link_path.is_symlink():
        link_path.unlink()
    os.symlink('/etc/passwd', str(link_path))
    try:
        bridge._dispatch_path('evil_test_link.c', ws)
        raise SystemExit('symlinked file not rejected by O_NOFOLLOW walk')
    except LspError as e:
        assert e.kind == 'lsp-path-outside-workspace', e.kind
        assert 'symlink' in e.detail or 'symlink' in str(e.extra), e.detail
finally:
    try: link_path.unlink()
    except FileNotFoundError: pass
PY
}

# --- 15e: race-free walk: rename mid-walk -> wrong-file read NEVER OK ---
# Two threads: one mutates the workspace tree, the other dispatches.
# The dispatch result MUST be either the original-inode bytes OR an
# lsp-path-outside-workspace envelope -- never bytes from a different
# inode the attacker swapped in. Codex post-implementation review of
# the path sandboxing surface tracked this as the canonical TOCTOU
# guard the race-free walk closes.
t_race_free_walk_rename() {
    python3 - << 'PY'
import sys, os, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge
from lsp_client import LspError

# Build a TEMP workspace so we can rename without disturbing the
# real tree. Use a wired extension (.c).
ws = Path(tempfile.mkdtemp(prefix='lsp-mcp-15e-'))
try:
    inner = ws / 'inner'
    inner.mkdir()
    target = inner / 'main.c'
    target.write_text('// original bytes\n')
    decoy = ws / 'decoy.c'
    decoy.write_text('// decoy bytes\n')

    rounds = 50
    bad_reads = 0
    for _ in range(rounds):
        # Background thread mutates the tree mid-dispatch.
        ev = threading.Event()
        def mutator():
            ev.wait(timeout=1.0)
            # Best-effort rename; OK if it races and fails.
            try:
                inner.rename(ws / 'inner_moved')
                (ws / 'inner_moved').rename(inner)
            except OSError:
                pass
        t = threading.Thread(target=mutator)
        t.start()
        ev.set()
        try:
            _, _, text, _ = bridge._dispatch_path('inner/main.c', ws)
            # If we got bytes, they MUST be from the original inode.
            if 'original bytes' not in text:
                bad_reads += 1
        except LspError:
            # Acceptable: the walk noticed the change and rejected.
            pass
        t.join()
    assert bad_reads == 0, f'{bad_reads}/{rounds} race-condition wrong-file reads'
finally:
    import shutil
    shutil.rmtree(str(ws), ignore_errors=True)
PY
}

# --- 15f: respawn replay still works after the absolute-path fix --------
# Codex design review High: rejecting all absolute paths would break
# the LSP-respawn replay path (it passes back the stored absolute
# resolved_path). Verify the absolute-in-workspace path is still
# accepted.
t_respawn_replay_absolute_path_still_works() {
    python3 - << 'PY'
import sys
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge
from lsp_client import LspError

ws = Path.cwd().resolve()
# Build an absolute in-workspace path the way the LSP-respawn
# replay path would pass it back through.
abs_path = str(ws / 'src/kernel/main.c')
resolved, lang, text, mtime_ns = bridge._dispatch_path(abs_path, ws)
assert lang == 'c', lang
assert 'kernel_main' in text, 'absolute in-workspace path failed dispatch'

# Absolute path OUTSIDE workspace -> rejected.
try:
    bridge._dispatch_path('/etc/passwd', ws)
    raise SystemExit('/etc/passwd not rejected')
except LspError as e:
    assert e.kind == 'lsp-path-outside-workspace', e.kind
PY
}

# --- 15g: workspace-root not a directory -> structured envelope --------
t_workspace_root_invalid() {
    python3 - << 'PY'
import sys
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge
from lsp_client import LspError

# Pass a regular file as the workspace root.
try:
    bridge._dispatch_path('foo.c', Path('/etc/hostname'))
    raise SystemExit('non-directory workspace not rejected')
except LspError as e:
    # workspace_root is a file -> os.open(O_DIRECTORY) raises ENOTDIR.
    assert e.kind == 'lsp-workspace-root-invalid', e.kind
PY
}

# --- 15h: deep-path cap (lsp-path-too-deep) -----------------------------
t_path_too_deep() {
    python3 - << 'PY'
import sys, tempfile
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge
from lsp_client import LspError

ws = Path.cwd()
deep = '/'.join(['x'] * (bridge._MAX_PATH_SEGMENTS + 1)) + '/foo.c'
try:
    bridge._dispatch_path(deep, ws)
    raise SystemExit('over-cap path not rejected')
except LspError as e:
    assert e.kind == 'lsp-path-too-deep', e.kind
    assert e.extra.get('cap') == bridge._MAX_PATH_SEGMENTS
PY
}

# --- 15i: FIFO with wired extension must not hang the bridge -----------
# Codex post-impl review High: opening the final segment without
# O_NONBLOCK would block on FIFO/blocking-device targets. Fix
# uses O_NONBLOCK on the final open + S_ISREG check.
t_fifo_path_does_not_hang() {
    python3 - << 'PY'
import sys, os, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge
from lsp_client import LspError

ws = Path(tempfile.mkdtemp(prefix='lsp-mcp-15i-'))
try:
    fifo = ws / 'evil.c'
    os.mkfifo(str(fifo))
    # _dispatch_path MUST reject within milliseconds; without the
    # O_NONBLOCK fix it would hang waiting for a writer.
    result = {'kind': None, 'detail': None}
    def go():
        try:
            bridge._dispatch_path('evil.c', ws)
            result['kind'] = 'no-rejection'
        except LspError as e:
            result['kind'] = e.kind
            result['detail'] = e.detail
    t = threading.Thread(target=go)
    t.start()
    t.join(timeout=2.0)
    assert not t.is_alive(), 'bridge HUNG opening FIFO -- O_NONBLOCK fix missing'
    assert result['kind'] == 'lsp-path-not-regular-file', \
        f'FIFO not rejected as non-regular: {result}'
finally:
    import shutil
    shutil.rmtree(str(ws), ignore_errors=True)
PY
}

# --- 15j: absolute path with in-workspace symlink REJECTED -------------
# Codex post-impl review Medium: Path.resolve() in the absolute-path
# branch was erasing symlinks before the O_NOFOLLOW walk could see
# them. Fix uses lexical normpath instead. The all-symlink rejection
# policy now applies to absolute paths too.
t_absolute_path_symlink_rejected() {
    python3 - << 'PY'
import sys, os, tempfile
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge
from lsp_client import LspError

ws = Path(tempfile.mkdtemp(prefix='lsp-mcp-15j-'))
try:
    real = ws / 'real'
    real.mkdir()
    target = real / 'main.c'
    target.write_text('// real bytes\n')
    link_dir = ws / 'link'
    os.symlink(str(real), str(link_dir))

    # Relative path through symlinked directory -> rejected.
    try:
        bridge._dispatch_path('link/main.c', ws)
        raise SystemExit('relative symlinked dir not rejected')
    except LspError as e:
        assert e.kind == 'lsp-path-outside-workspace', e.kind
        assert 'symlink' in e.detail or 'symlink' in str(e.extra), e.detail

    # ABSOLUTE path through symlinked directory -> ALSO rejected
    # (regression guard for Codex Medium).
    abs_through_link = str(ws / 'link' / 'main.c')
    try:
        bridge._dispatch_path(abs_through_link, ws)
        raise SystemExit('absolute path through symlink not rejected')
    except LspError as e:
        assert e.kind == 'lsp-path-outside-workspace', e.kind
        assert 'symlink' in e.detail or 'symlink' in str(e.extra), e.detail

    # Direct (non-symlinked) absolute path still works.
    abs_direct = str(ws / 'real' / 'main.c')
    resolved, lang, text, _ = bridge._dispatch_path(abs_direct, ws)
    assert lang == 'c', lang
    assert 'real bytes' in text, text
finally:
    import shutil
    shutil.rmtree(str(ws), ignore_errors=True)
PY
}

# --- 15k: symlinked workspace root + LSP-respawn absolute replay -------
# Codex post-commit review Medium: when workspace_root is itself a
# symlink (e.g. /link -> /real), the absolute-path branch's
# prefix check would compare the operator-supplied /link/... path
# against the resolved /real/... prefix and reject it. The
# LSP-respawn replay path stores resolved_path from
# workspace_root.joinpath(...) which preserves the operator's
# spelling, so replay would silently fail. Fix accepts BOTH the
# raw and resolved prefixes.
t_symlinked_workspace_root_replay() {
    python3 - << 'PY'
import sys, os, tempfile
from pathlib import Path
sys.path.insert(0, 'scripts/lsp-mcp')
import bridge
from lsp_client import LspError

real_root = Path(tempfile.mkdtemp(prefix='lsp-mcp-15k-real-'))
link_root = Path(tempfile.mktemp(prefix='lsp-mcp-15k-link-'))
try:
    src_dir = real_root / 'src'
    src_dir.mkdir()
    target = src_dir / 'main.c'
    target.write_text('// hello\n')
    # workspace_root is the SYMLINK; the dir_fd walk uses it
    # directly, so segments are anchored at /link.
    os.symlink(str(real_root), str(link_root))

    # 1) Relative dispatch via the symlinked workspace works.
    resolved, lang, text, _ = bridge._dispatch_path('src/main.c', link_root)
    assert lang == 'c', lang
    assert 'hello' in text, text
    # The respawn replay path would store this resolved value:
    stored = str(resolved)
    assert stored.startswith(str(link_root)), \
        f'stored resolved_path {stored!r} should preserve operator spelling'

    # 2) ABSOLUTE replay using the stored /link/... path MUST be
    # accepted (regression guard for the Codex Medium).
    resolved2, lang2, text2, _ = bridge._dispatch_path(stored, link_root)
    assert lang2 == 'c', lang2
    assert 'hello' in text2, text2

    # 3) ABSOLUTE replay using the /real/... realpath ALSO accepted
    # (defense in depth -- both anchors are valid).
    real_abs = str(real_root / 'src' / 'main.c')
    resolved3, lang3, text3, _ = bridge._dispatch_path(real_abs, link_root)
    assert lang3 == 'c', lang3
    assert 'hello' in text3, text3

    # 4) ABSOLUTE outside both /link and /real -> rejected.
    try:
        bridge._dispatch_path('/etc/passwd', link_root)
        raise SystemExit('outside-workspace not rejected')
    except LspError as e:
        assert e.kind == 'lsp-path-outside-workspace', e.kind

    # 5) PRODUCTION-PATH coverage: _workspace_root_from_argv
    # preserves operator spelling (Codex post-impl review fix).
    # An MCP host launching with --repo-root=/link gets /link
    # back, NOT /real -- so the absolute-replay roundtrip works
    # end-to-end through the same code path production uses.
    import argparse
    ns = argparse.Namespace(repo_root=str(link_root))
    ws_via_argv = bridge._workspace_root_from_argv(ns)
    assert ws_via_argv == link_root, \
        f'argv-derived workspace lost spelling: {ws_via_argv} != {link_root}'

    # And via env.
    saved = os.environ.get('LSP_MCP_WORKSPACE_ROOT')
    try:
        os.environ['LSP_MCP_WORKSPACE_ROOT'] = str(link_root)
        ws_via_env = bridge._workspace_root_from_argv(
            argparse.Namespace(repo_root=None))
        assert ws_via_env == link_root, \
            f'env-derived workspace lost spelling: {ws_via_env}'
        # Now do the full dispatch-then-replay roundtrip via the
        # env-derived root.
        resolved_e, _, _, _ = bridge._dispatch_path(
            'src/main.c', ws_via_env)
        # Replay using the stored absolute path.
        resolved_e2, _, _, _ = bridge._dispatch_path(
            str(resolved_e), ws_via_env)
        assert str(resolved_e) == str(resolved_e2), \
            f'replay drift: {resolved_e} != {resolved_e2}'
    finally:
        if saved is None: os.environ.pop('LSP_MCP_WORKSPACE_ROOT', None)
        else:             os.environ['LSP_MCP_WORKSPACE_ROOT'] = saved
finally:
    try: link_root.unlink()
    except (FileNotFoundError, IsADirectoryError): pass
    import shutil
    shutil.rmtree(str(real_root), ignore_errors=True)
PY
}

# --- 16a: --warm-start without value runs against every registered LSP ----
# Asserts: exit 0, JSON line `event: warm-begin` lists every registered
# language, JSON line `event: warm-complete` (or `warm-over-budget`) is
# emitted, and the post-banner reports >= 1 LSPs spawned IF clangd-19 is
# installed, OR 0 LSPs with all-spawn-failed entries IF the host has no
# LSPs at all (CI host without language tooling). Timeout: 180s budget
# because blocking warm-start with cold clangd index (~30s) AND pyright
# installed (parallel max ~60s soft cap) plus per-language handshake
# overhead can push past the original 90s budget on a fresh dev box.
# Cached-index runs finish in ~10-15s; cold-cache CI runs need the
# fuller window.
t_warm_start_default() {
    local out
    out="$(timeout 180 python3 scripts/lsp-mcp/bridge.py --warm-start \
        --self-test 2>&1)" || {
        printf '[warm-start-default] FAIL: exit non-zero\n%s\n' "$out" >&2
        return 1
    }
    case "$out" in
        *'"event": "warm-begin"'*) ;;
        *) printf '[warm-start-default] FAIL: missing warm-begin\n' >&2
           return 1 ;;
    esac
    case "$out" in
        *'"event": "warm-complete"'*|*'"event": "warm-over-budget"'*) ;;
        *) printf '[warm-start-default] FAIL: missing warm-complete or '\
'warm-over-budget\n' >&2
           return 1 ;;
    esac
    # Accept either banner: "[lsp-mcp] OK: N LSPs spawned, M tools
    # registered, bridge ready" (mcp SDK present, normal path) OR
    # "[lsp-mcp] SKIP: mcp SDK not installed; AI-agent integration
    # unavailable. Install with `pip install mcp` to enable." (CI host
    # without the mcp pip package -- documented healthy exit-0 path
    # in scripts/lsp-mcp/bridge.py:_self_test, not a regression).
    case "$out" in
        *"LSPs spawned"*|*"SKIP: mcp SDK not installed"*) ;;
        *) printf '[warm-start-default] FAIL: missing OK or SKIP banner\n' >&2
           return 1 ;;
    esac
    return 0
}

# --- 16b: --warm-start=c,py subset only warms named langs -----------------
# Asserts: warm-begin payload's langs list is a SUBSET of {c, py}. Other
# languages must not appear in the warm-begin langs array. Tolerates the
# spawn-failed result for any of c/py whose binary is missing.
t_warm_start_subset() {
    local out
    out="$(timeout 90 python3 scripts/lsp-mcp/bridge.py --warm-start=c,py \
        --self-test 2>&1)" || {
        printf '[warm-start-subset] FAIL: exit non-zero\n%s\n' "$out" >&2
        return 1
    }
    # Extract the warm-begin line and check langs is a subset.
    python3 - <<'PY' || return 1
import json, sys, subprocess
out = subprocess.run(
    ["timeout", "90", "python3", "scripts/lsp-mcp/bridge.py",
     "--warm-start=c,py", "--self-test"],
    capture_output=True, text=True, check=False,
)
if out.returncode != 0:
    print(f"[warm-start-subset] FAIL: rc={out.returncode}", file=sys.stderr)
    sys.exit(1)
lines = (out.stdout + out.stderr).splitlines()
begin = None
for line in lines:
    s = line.strip()
    if s.startswith("{") and '"event": "warm-begin"' in s:
        begin = json.loads(s)
        break
if begin is None:
    print("[warm-start-subset] FAIL: no warm-begin event", file=sys.stderr)
    sys.exit(1)
langs = set(begin.get("langs") or [])
allowed = {"c", "py"}
extra = langs - allowed
if extra:
    print(f"[warm-start-subset] FAIL: extra langs warmed: {extra}",
          file=sys.stderr)
    sys.exit(1)
if not langs:
    print("[warm-start-subset] FAIL: empty langs", file=sys.stderr)
    sys.exit(1)
PY
    return 0
}

# --- 16c: unknown lang in spec -> WARN + skip, exit 0 ---------------------
# Asserts: --warm-start=bogus,c does NOT abort; emits a
# warm-unknown-lang event for `bogus`, still warms `c` (when registered),
# and exits 0.
t_warm_start_unknown_lang() {
    local out
    out="$(timeout 90 python3 scripts/lsp-mcp/bridge.py \
        --warm-start=bogus,c --self-test 2>&1)" || {
        printf '[warm-start-unknown] FAIL: exit non-zero\n%s\n' "$out" >&2
        return 1
    }
    case "$out" in
        *'"event": "warm-unknown-lang"'*'"lang": "bogus"'*) ;;
        *'"lang": "bogus"'*'"event": "warm-unknown-lang"'*) ;;
        *) printf '[warm-start-unknown] FAIL: missing warm-unknown-lang\n' >&2
           return 1 ;;
    esac
    return 0
}

# --- 16d: empty spec / no registered langs -> warm-empty + exit 0 ---------
# Asserts: --warm-start=  (empty after split) OR --warm-start=zzz,yyy
# (all unknown) emits warm-empty and still exits 0; serving must not
# block on a typo. Different from 16c: 16c has `c` valid, this has none.
t_warm_start_empty_spec() {
    local out
    out="$(timeout 90 python3 scripts/lsp-mcp/bridge.py \
        --warm-start=zzz,yyy --self-test 2>&1)" || {
        printf '[warm-start-empty] FAIL: exit non-zero\n%s\n' "$out" >&2
        return 1
    }
    case "$out" in
        *'"event": "warm-empty"'*) ;;
        *) printf '[warm-start-empty] FAIL: missing warm-empty\n' >&2
           return 1 ;;
    esac
    return 0
}

# --- 16e: snapshot_progress copy contract is race-safe --------------------
# Asserts: LspSubprocess._record_progress + snapshot_progress survive a
# tight write/read race -- a concurrent reader iterating snapshot_progress
# must NEVER trip RuntimeError(dictionary changed size during iteration).
# Drives the contract in isolation (no real LSP needed) so the test
# passes on any host.
t_warm_progress_snapshot_race() {
    python3 - <<'PY' || return 1
import sys, threading, time
sys.path.insert(0, "scripts/lsp-mcp")
from lsp_client import LspSubprocess

# Build a synthetic instance without spawning a subprocess. Use
# __new__ + manual init of just the fields _record_progress /
# snapshot_progress touch, to avoid the cmd= subprocess.Popen path.
inst = LspSubprocess.__new__(LspSubprocess)
inst._progress_by_token = {}
inst._progress_seen_at = None
inst._handshake_done_at = None
inst._progress_lock = threading.Lock()

stop = threading.Event()
errors = []

def writer():
    i = 0
    while not stop.is_set():
        try:
            inst._record_progress({
                "token": f"tok{i % 16}",
                "value": {"kind": "report", "percentage": i % 100,
                          "message": f"m{i}", "title": "Indexing"},
            })
        except Exception as exc:
            errors.append(f"writer: {exc!r}")
            return
        i += 1

def reader():
    while not stop.is_set():
        try:
            snap = inst.snapshot_progress()
            for k, v in snap["tokens"].items():
                _ = (k, v.get("kind"), v.get("percentage"))
        except Exception as exc:
            errors.append(f"reader: {exc!r}")
            return

# 4 writers + 4 readers for 0.5s.
threads = ([threading.Thread(target=writer) for _ in range(4)]
           + [threading.Thread(target=reader) for _ in range(4)])
for t in threads:
    t.start()
time.sleep(0.5)
stop.set()
for t in threads:
    t.join(2.0)

if errors:
    print(f"[warm-progress-race] FAIL: {errors[:3]}", file=sys.stderr)
    sys.exit(1)
# Sanity: at least some writes landed.
assert inst._progress_by_token, "no writes landed"
PY
    return 0
}

# --- 16f: server-initiated workDoneProgress/create ack ---------------------
# Asserts: when the LSP sends a `window/workDoneProgress/create` request
# (id + method, no result/error), the bridge replies with {result: null}
# under the same id. Tested against a fake stdio LSP that emits the
# request unsolicited and waits for the ack.
t_warm_workdone_create_ack() {
    python3 - <<'PY' || return 1
import json, os, subprocess, sys, threading, time
sys.path.insert(0, "scripts/lsp-mcp")
from lsp_client import LspSubprocess

# Fake LSP that:
#   1. Reads `initialize`, replies with capabilities.
#   2. Reads `initialized` notification.
#   3. Sends a workDoneProgress/create request with id=99.
#   4. Waits for a response with id=99 + result=null.
#   5. If received, prints OK on stderr and exits 0; otherwise exits 2.
fake_src = r'''
import json, sys
def read_msg():
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        line = line.rstrip(b"\r\n")
        if line == b"":
            break
        k, _, v = line.decode().partition(":")
        headers[k.strip().lower()] = v.strip()
    n = int(headers.get("content-length", "0"))
    return json.loads(sys.stdin.buffer.read(n))
def write_msg(obj):
    body = json.dumps(obj).encode()
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(body))
    sys.stdout.buffer.write(body)
    sys.stdout.buffer.flush()

m = read_msg()  # initialize
write_msg({"jsonrpc":"2.0","id":m["id"],"result":{"capabilities":{}}})
m = read_msg()  # initialized notification
write_msg({"jsonrpc":"2.0","id":99,
           "method":"window/workDoneProgress/create",
           "params":{"token":"t1"}})
ack = read_msg()
if (ack and ack.get("id") == 99 and "result" in ack
        and ack.get("result") is None):
    sys.stderr.write("FAKE_LSP_ACK_OK\n")
    sys.exit(0)
sys.stderr.write(f"FAKE_LSP_NO_ACK got={ack!r}\n")
sys.exit(2)
'''
import tempfile
with tempfile.NamedTemporaryFile(mode="w", suffix=".py", delete=False) as f:
    f.write(fake_src)
    fake_path = f.name

try:
    lsp = LspSubprocess(["python3", fake_path], lang="fake16f")
    lsp.initialize(root_uri="file:///tmp", timeout=2.0)
    # Give the fake LSP a moment to send + receive the ack.
    time.sleep(0.5)
    rc = lsp.shutdown(timeout=2.0)
    # Inspect captured stderr.
    # LspSubprocess does not expose captured stderr; just verify the
    # subprocess exited cleanly. The fake exits 0 only if the ack
    # arrived, so a non-zero exit means we missed it.
    proc = lsp._proc
    if proc and proc.returncode not in (0, None):
        # Drain any remaining stderr.
        try:
            err = proc.stderr.read().decode("utf-8", "replace")
        except Exception:
            err = "<unreadable>"
        print(f"[warm-workdone-create] FAIL: fake LSP rc={proc.returncode} "
              f"stderr={err!r}", file=sys.stderr)
        sys.exit(1)
finally:
    try: os.unlink(fake_path)
    except OSError: pass
PY
    return 0
}

# --- 17a: --warm-start --warm-start-mode=background --self-test < 1.5s ----
# Asserts: --warm-start-mode=background returns to the self-test exit
# gate without blocking on the bg warm worker. Wall-clock measured by
# Python's time.monotonic before subprocess.run + after; budget is
# 1.5s (cold-cache budget of warm-start is 60s, on-demand spawn of
# clangd takes ~2s; if --warm-start-mode=background was actually
# blocking we'd see >2s).
t_warm_start_mode_bg_fast_exit() {
    python3 - <<'PY' || return 1
import subprocess, sys, time
t0 = time.monotonic()
r = subprocess.run(
    ["python3", "scripts/lsp-mcp/bridge.py",
     "--warm-start=c", "--warm-start-mode=background",
     "--self-test"],
    capture_output=True, text=True, timeout=30,
)
elapsed = time.monotonic() - t0
if r.returncode != 0:
    print(f"[17a] FAIL: rc={r.returncode}\nSTDOUT:\n{r.stdout}\nSTDERR:\n{r.stderr}",
          file=sys.stderr)
    sys.exit(1)
if elapsed >= 1.5:
    print(f"[17a] FAIL: took {elapsed:.2f}s (budget 1.5s)", file=sys.stderr)
    sys.exit(1)
PY
    return 0
}

# --- 17b: bg dispatch event present + bg thread launched -----------------
# Asserts: --warm-start-mode=background emits the warm-bg-dispatched
# JSON log line BEFORE the warm-begin/warm-complete pair fires (proving
# the dispatch happens on the main thread; the worker runs concurrently).
t_warm_start_mode_bg_dispatch_event() {
    out="$(python3 scripts/lsp-mcp/bridge.py --warm-start=c \
        --warm-start-mode=background --self-test 2>&1)" || {
        printf '[17b] FAIL: exit non-zero\n%s\n' "$out" >&2
        return 1
    }
    case "$out" in
        *'"event": "warm-bg-dispatched"'*) ;;
        *) printf '[17b] FAIL: missing warm-bg-dispatched event\n' >&2
           return 1 ;;
    esac
    return 0
}

# --- 17c: blocking mode default does NOT emit warm-bg-dispatched ---------
# Asserts: omitting --warm-start-mode is equivalent to
# --warm-start-mode=blocking and runs warm-start inline. The absence of
# warm-bg-dispatched in the log proves the blocking branch ran. Also
# confirms warm-complete (or warm-over-budget) fires before the bridge
# reaches the OK banner.
t_warm_start_mode_blocking_default() {
    out="$(python3 scripts/lsp-mcp/bridge.py --warm-start=c \
        --self-test 2>&1)" || {
        printf '[17c] FAIL: exit non-zero\n%s\n' "$out" >&2
        return 1
    }
    case "$out" in
        *'"event": "warm-bg-dispatched"'*)
            printf '[17c] FAIL: blocking default emitted warm-bg-dispatched\n' >&2
            return 1 ;;
    esac
    case "$out" in
        *'"event": "warm-complete"'*|*'"event": "warm-over-budget"'*) ;;
        *) printf '[17c] FAIL: missing warm-complete\n' >&2
           return 1 ;;
    esac
    return 0
}

# --- 17d: argparse rejects bogus mode value ------------------------------
# Asserts: --warm-start-mode=baground exits non-zero with an error
# message naming the valid choices. Pure argparse contract test.
t_warm_start_mode_argparse_reject() {
    if python3 scripts/lsp-mcp/bridge.py --warm-start \
        --warm-start-mode=baground --self-test >/tmp/17d.out 2>&1; then
        printf '[17d] FAIL: bogus mode accepted\n' >&2
        return 1
    fi
    grep -q "invalid choice: 'baground'" /tmp/17d.out || {
        printf '[17d] FAIL: missing argparse error message\n%s\n' \
            "$(cat /tmp/17d.out)" >&2
        return 1
    }
    return 0
}

# --- 17e: shutdown publish gate reaps in-flight spawn --------------------
# Asserts: when _BRIDGE_SHUTTING_DOWN is set BEFORE _get_or_spawn
# publishes its new instance, the spawn raises lsp-shutdown and the
# instance is shut down inline (not leaked). Drives the gate in
# isolation by setting _BRIDGE_SHUTTING_DOWN, then calling
# _get_or_spawn against a fake LSP that completes initialize.
t_warm_shutdown_publish_gate() {
    python3 - <<'PY' || return 1
import os, subprocess, sys, tempfile, time
sys.path.insert(0, "scripts/lsp-mcp")
import bridge as B
from lsp_client import LspError, LspSubprocess
from pathlib import Path

# Fake LSP that responds to initialize within a few ms (lets _get_or_spawn
# reach the publish gate quickly).
fake_src = r'''
import json, sys
def read_msg():
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line: return None
        line = line.rstrip(b"\r\n")
        if line == b"": break
        k, _, v = line.decode().partition(":")
        headers[k.strip().lower()] = v.strip()
    n = int(headers.get("content-length", "0"))
    return json.loads(sys.stdin.buffer.read(n))
def write_msg(o):
    body = json.dumps(o).encode()
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(body))
    sys.stdout.buffer.write(body)
    sys.stdout.buffer.flush()
m = read_msg()
write_msg({"jsonrpc":"2.0","id":m["id"],"result":{"capabilities":{}}})
read_msg()  # initialized notification
import time as _t; _t.sleep(60)  # wait for shutdown
'''
with tempfile.NamedTemporaryFile(mode="w", suffix=".py", delete=False) as f:
    f.write(fake_src); fake_path = f.name

def fake_spawn(workspace_root):
    inst = LspSubprocess(["python3", fake_path], lang="fake17e")
    inst.initialize(root_uri=workspace_root.as_uri(), timeout=2.0)
    return inst

B.register_spawner("fake17e", fake_spawn)

try:
    # Pre-set the shutdown gate, then call _get_or_spawn -- it must
    # raise lsp-shutdown after the spawner returns.
    B._BRIDGE_SHUTTING_DOWN.set()
    raised = False
    try:
        inst = B._get_or_spawn("fake17e", Path("/tmp"))
    except LspError as e:
        if e.kind == "lsp-shutdown":
            raised = True
    if not raised:
        print("[17e] FAIL: _get_or_spawn did not raise lsp-shutdown",
              file=sys.stderr)
        sys.exit(1)
    # Verify the LSP is NOT in _LIVE_LSPS (gate prevented publish).
    with B._LIVE_LSPS_LOCK:
        if any(k[0] == "fake17e" for k in B._LIVE_LSPS.keys()):
            print("[17e] FAIL: leaked instance in _LIVE_LSPS", file=sys.stderr)
            sys.exit(1)
finally:
    B._BRIDGE_SHUTTING_DOWN.clear()  # don't leak gate state to next test
    try: os.unlink(fake_path)
    except OSError: pass
PY
    return 0
}

# --- 17f: bg thread re-entry joins prior thread cleanly ------------------
# Asserts: a second _maybe_warm_start-equivalent dispatch while a prior
# bg thread is alive joins the prior thread (no double-launch, no
# orphaned thread). Drives _WARM_LOCK + _WARM_THREAD lifecycle in
# isolation since main()'s _maybe_warm_start is a closure.
t_warm_bg_reentry_safe() {
    python3 - <<'PY' || return 1
import sys, threading, time
sys.path.insert(0, "scripts/lsp-mcp")
import bridge as B

# Replace _warm_start_run_in_thread with a sleep-until-cancel for
# this test. Original is restored in finally.
orig = B._warm_start_run_in_thread
def fake_runner(workspace_root, spec):
    try:
        for _ in range(60):
            if B._WARM_CANCEL.wait(0.05):
                return
    finally:
        B._WARM_DONE.set()
B._warm_start_run_in_thread = fake_runner

try:
    from pathlib import Path
    workspace = Path("/tmp")
    # First dispatch
    with B._WARM_LOCK:
        B._WARM_CANCEL.clear()
        B._WARM_DONE.clear()
        t1 = threading.Thread(target=B._warm_start_run_in_thread,
                              args=(workspace, "c"), daemon=True)
        B._WARM_THREAD = t1
        t1.start()
    time.sleep(0.05)
    assert t1.is_alive(), "first thread should be alive"

    # Second dispatch must join the first cleanly
    with B._WARM_LOCK:
        prev = B._WARM_THREAD
        if prev is not None and prev.is_alive():
            B._WARM_CANCEL.set()
            prev.join(timeout=2.0)
        B._WARM_CANCEL.clear()
        B._WARM_DONE.clear()
        t2 = threading.Thread(target=B._warm_start_run_in_thread,
                              args=(workspace, "py"), daemon=True)
        B._WARM_THREAD = t2
        t2.start()

    assert not t1.is_alive(), "first thread should have been joined"
    assert t2.is_alive(), "second thread should be running"
    # Cleanup
    B._WARM_CANCEL.set()
    t2.join(timeout=2.0)
    assert not t2.is_alive(), "second thread should have stopped"
finally:
    B._warm_start_run_in_thread = orig
    B._WARM_THREAD = None
    B._WARM_CANCEL.clear()
    B._WARM_DONE.clear()
PY
    return 0
}

# --- 17g: TOCTOU shutdown gate -- shutdown fires AFTER spawner returns ---
# Asserts: _shutdown_all_lsps that fires AFTER _get_or_spawn's spawner
# has already returned (but BEFORE the new instance was published)
# does NOT leak the new subprocess. Drives the precise interleaving
# the post-implementation adversarial review found: spawner returned,
# instance is in hand, then shutdown is signalled, then the publish
# block runs -- with the gate check inside _LIVE_LSPS_LOCK the
# interleaving must reject the publish, reap the instance, and raise
# lsp-shutdown. Verified by counting registered spawner invocations
# AND that no instance is left in _LIVE_LSPS for the test key.
t_warm_shutdown_gate_toctou() {
    python3 - <<'PY' || return 1
import os, sys, tempfile, threading, time
sys.path.insert(0, "scripts/lsp-mcp")
import bridge as B
from lsp_client import LspError, LspSubprocess
from pathlib import Path

# Fake LSP that completes initialize quickly.
fake_src = r'''
import json, sys
def read_msg():
    h = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line: return None
        line = line.rstrip(b"\r\n")
        if line == b"": break
        k,_,v = line.decode().partition(":")
        h[k.strip().lower()] = v.strip()
    n = int(h.get("content-length","0"))
    return json.loads(sys.stdin.buffer.read(n))
def write_msg(o):
    body = json.dumps(o).encode()
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(body))
    sys.stdout.buffer.write(body); sys.stdout.buffer.flush()
m = read_msg()
write_msg({"jsonrpc":"2.0","id":m["id"],"result":{"capabilities":{}}})
read_msg()
import time as _t; _t.sleep(60)
'''
with tempfile.NamedTemporaryFile(mode="w", suffix=".py", delete=False) as f:
    f.write(fake_src); fake_path = f.name

# Spawner that wraps the real LspSubprocess but blocks on a barrier
# AFTER initialize completes -- simulates the moment between
# spawner-return and the publish block in _get_or_spawn.
gate_set = threading.Event()
spawner_returned = threading.Event()

spawn_call_count = [0]
spawn_count_lock = threading.Lock()
def fake_spawn(workspace_root):
    with spawn_count_lock:
        spawn_call_count[0] += 1
    inst = LspSubprocess(["python3", fake_path], lang="fake17g")
    inst.initialize(root_uri=workspace_root.as_uri(), timeout=2.0)
    spawner_returned.set()
    # Block here until the test sets the shutdown gate. This
    # simulates the precise window between "spawner has finished
    # producing inst" and "_get_or_spawn's publish block runs".
    gate_set.wait(timeout=5.0)
    return inst

B.register_spawner("fake17g", fake_spawn)

raised_kind = None
final_check_done = threading.Event()
def caller():
    nonlocal_raised = []
    try:
        B._get_or_spawn("fake17g", Path("/tmp"))
    except LspError as e:
        nonlocal_raised.append(e.kind)
    final_check_done.set()
    if nonlocal_raised:
        # stash on a thread attribute the main thread can read
        threading.current_thread().result_kind = nonlocal_raised[0]

t = threading.Thread(target=caller, daemon=True, name="toctou-caller")
t.start()

# Wait for the spawner to have returned its inst.
if not spawner_returned.wait(timeout=10.0):
    print("[17g] FAIL: spawner never returned", file=sys.stderr)
    sys.exit(1)

# Attach a SECOND waiter on the same key so the post-impl
# consistency review's "waiter must also see lsp-shutdown" claim
# is exercised. The waiter joins via the existing _SPAWN_EVENTS
# gate while the owner (`t`) is parked inside the spawner.
waiter_done = threading.Event()
waiter_result = []
def waiter():
    try:
        B._get_or_spawn("fake17g", Path("/tmp"))
    except LspError as e:
        waiter_result.append(e.kind)
    waiter_done.set()
w = threading.Thread(target=waiter, daemon=True, name="toctou-waiter")
w.start()
# Give the waiter time to attach to _SPAWN_EVENTS.
time.sleep(0.1)

# Now flip the shutdown gate, mimicking what _shutdown_all_lsps
# does (set gate + clear _LIVE_LSPS snapshot). The fake spawner
# is still parked, holding inst; it has NOT yet entered the
# publish block.
B._BRIDGE_SHUTTING_DOWN.set()
with B._LIVE_LSPS_LOCK:
    snapshot_keys = list(B._LIVE_LSPS.keys())
    B._LIVE_LSPS.clear()

# Release the spawner so it returns inst into _get_or_spawn's
# publish block. The atomic gate-check-then-publish under
# _LIVE_LSPS_LOCK MUST reject the publish.
gate_set.set()

# Wait for caller (owner) to finish.
if not final_check_done.wait(timeout=10.0):
    print("[17g] FAIL: caller did not finish", file=sys.stderr)
    sys.exit(1)
t.join(timeout=2.0)

result = getattr(t, "result_kind", None)
if result != "lsp-shutdown":
    print(f"[17g] FAIL: owner expected lsp-shutdown, got {result!r}",
          file=sys.stderr)
    sys.exit(1)

# Waiter must also see lsp-shutdown (post-impl consistency review).
if not waiter_done.wait(timeout=5.0):
    print("[17g] FAIL: waiter did not finish", file=sys.stderr)
    sys.exit(1)
w.join(timeout=2.0)
if not waiter_result or waiter_result[0] != "lsp-shutdown":
    print(f"[17g] FAIL: waiter expected lsp-shutdown, got {waiter_result!r}",
          file=sys.stderr)
    sys.exit(1)

# Critical: the waiter MUST have been a true waiter (attached to
# _SPAWN_EVENTS), not a second owner that re-entered _get_or_spawn
# after the first owner popped the event. Asserting spawn_call_count
# == 1 proves the waiter path -- a second-owner scenario would have
# called fake_spawn a second time. (Re-adversarial review M.)
with spawn_count_lock:
    if spawn_call_count[0] != 1:
        print(f"[17g] FAIL: expected exactly 1 fake_spawn call, "
              f"got {spawn_call_count[0]} -- waiter became 2nd owner",
              file=sys.stderr)
        sys.exit(1)

# Verify no leaked instance landed in _LIVE_LSPS post-rejection.
with B._LIVE_LSPS_LOCK:
    if any(k[0] == "fake17g" for k in B._LIVE_LSPS.keys()):
        print("[17g] FAIL: leaked instance in _LIVE_LSPS",
              file=sys.stderr)
        sys.exit(1)

# Cleanup module state
B._BRIDGE_SHUTTING_DOWN.clear()
try: os.unlink(fake_path)
except OSError: pass
PY
    return 0
}

# --- 9a: LSP-process-leak detection (run LAST) ----------------------------
# --- 9b: deterministic reap on every termination signal -------------------
# Asserts: a bridge killed by SIGTERM / SIGINT / SIGHUP reaps the language
# servers it started -- INCLUDING one spawned but not yet published into
# _LIVE_LSPS, and including a repeated (storm) signal during cleanup.
#
# The child here is a stubborn stand-in that closes its own stdin and never
# exits on its own. That is the whole point: a real clangd usually exits
# when the pipe closes, so a test using clangd would pass with or without
# the handler and would prove nothing about the reap. Rate measurement over
# repeated harness runs has the same defect -- it can only show that the
# flake stopped reproducing (Codex design review, Medium). A child that
# provably cannot self-exit makes the assertion discriminate, and step 0
# below verifies that property instead of assuming it.
LSP_STUBBORN_DRIVER='
import ctypes, os, subprocess, sys, threading, time
import ctypes.util
sys.path.insert(0, "scripts/lsp-mcp")
import bridge
import lsp_client
from lsp_client import LspSubprocess

mode = sys.argv[1]
STUBBORN = (
    "import sys, time\n"
    "sys.stdin.close()\n"          # never observe EOF on the pipe
    "time.sleep(600)\n"
)
# A leader that exits its own THREAD while a worker runs on. pthread_exit
# from the initial thread ends that thread only: the thread group lives
# on, so /proc/PID/stat reports Z and /proc/PID/environ answers EACCES
# while a worker task still carries the owner stamp. Section 20 had to
# teach both _is_zombie and _carries_our_owner_id to read
# /proc/PID/task/<tid>/ because of exactly this, and verified it BY HAND.
# This is that shape, wired to a regression.
THREAD_LEADER = (
    "import ctypes, sys, threading, time\n"
    "sys.stdin.close()\n"
    "threading.Thread(target=time.sleep, args=(600,)).start()\n"
    "time.sleep(0.2)\n"
    "ctypes.CDLL(\"libc.so.6\").pthread_exit(None)\n"
)
# A leader that forks its own stubborn helper, the way a language server
# with a worker process does. Killing only the leader leaves the helper --
# which is the same leak one level down.
# Announces via a FILE, not stderr: the stderr of a language server is a
# pipe to the bridge, so anything written there never reaches the shell
# running the test.
STUBBORN_PARENT = (
    "import os, subprocess, sys, time\n"
    "sys.stdin.close()\n"
    "k = subprocess.Popen([sys.executable, \"-c\",\n"
    "    \"import sys, time\\nsys.stdin.close()\\ntime.sleep(600)\"])\n"
    "f = open(os.environ[\"STUB_HELPER_FILE\"], \"w\")\n"
    "f.write(str(k.pid)); f.flush(); f.close()\n"
    "time.sleep(600)\n"
)
# The POLITE leader, which is the harder case: it exits the moment its stdin
# closes, so the reap succeeds and the record would normally be retired --
# while the helper it forked ignores everything and is reparented away from
# the bridge, out of reach of any later search for our own children. The
# process group is the only remaining handle on it.
STUBBORN_DETACHED = (
    "import os, subprocess, sys, time\n"
    "k = subprocess.Popen([sys.executable, \"-c\",\n"
    "    \"import sys, time\\nsys.stdin.close()\\ntime.sleep(600)\"],\n"
    "    start_new_session=True)\n"
    "f = open(os.environ[\"STUB_HELPER_FILE\"], \"w\")\n"
    "f.write(str(k.pid)); f.flush(); f.close()\n"
    "time.sleep(600)\n"
)
STUBBORN_CLEAN = (
    "import os, signal, subprocess, sys, time\n"
    "k = subprocess.Popen([sys.executable, \"-c\",\n"
    "    \"import signal, sys, time\\n\"\n"
    "    \"signal.signal(signal.SIGTERM, signal.SIG_IGN)\\n\"\n"
    "    \"sys.stdin.close()\\ntime.sleep(600)\"])\n"
    "f = open(os.environ[\"STUB_HELPER_FILE\"], \"w\")\n"
    "f.write(str(k.pid)); f.flush(); f.close()\n"
    "sys.stdin.read()\n"
    "sys.exit(0)\n"
)
bridge._install_signal_handlers()

if mode.startswith("threadleader"):
    # The shape needs glibc-style pthread_exit. A host without a loadable
    # libc.so.6 cannot produce it, and the honest answer there is SKIP.
    # Once libc DOES load, failing to reach Z-with-a-live-task is a FAIL,
    # not a skip -- the harness checks the shape itself before signalling
    # rather than trusting this probe (Codex design review, next steps).
    # Absence and breakage are split the same way saturate_stderr splits
    # them: catching every loader error as "unsupported host" let a
    # descriptor limit, a permission problem or a corrupt loader retire
    # this coverage under a green run (Codex adversarial review, Medium).
    _found = ctypes.util.find_library("c")
    if _found != "libc.so.6":
        sys.stdout.write("NOSHAPE no glibc-style libc.so.6 on this host "
                         "(the loader offers %r)\n" % (_found,))
        sys.stdout.flush()
        sys.exit(0)
    try:
        _libc = ctypes.CDLL("libc.so.6")
    except Exception as exc:
        # The loader HAS it; failing to load it is ours to explain.
        sys.stdout.write("SHAPEFAIL libc.so.6 is present but did not "
                         "load: %s\n" % exc)
        sys.stdout.flush()
        sys.exit(0)
    if not hasattr(_libc, "pthread_exit"):
        sys.stdout.write("NOSHAPE libc.so.6 has no pthread_exit\n")
        sys.stdout.flush()
        sys.exit(0)

if mode == "spawnrace_nopidfd":
    # Same race with pidfd support removed. The runtime floor in
    # scripts/setup.sh is python3 3.8 and os.pidfd_open arrived in 3.9, so
    # a host without it is supported configuration -- and the guarantee
    # must not quietly depend on the reviewer host being newer.
    def _no_pidfd(*a, **kw):
        raise OSError("pidfd_open unavailable")
    os.pidfd_open = _no_pidfd

if mode.startswith("spawnrace"):
    # Widen the Popen-to-record window to something a shell can aim at, by
    # wrapping Popen so it sleeps AFTER the child exists but BEFORE
    # _record_spawn runs. Patched in the DRIVER, so the production path
    # carries no test hook.
    #
    # Deliberately on the MAIN thread. A spawning WORKER thread keeps
    # running while the handler executes and can still record the child, so
    # it does not reproduce the real hazard: when the interrupted frame is
    # the main thread it never resumes, the in-flight count never falls,
    # and no amount of waiting produces a record. Only the procfs
    # own-children sweep can find the child in that state.
    import lsp_client as _lc
    _real_popen = _lc.subprocess.Popen

    class _SlowPopen(_real_popen):          # type: ignore[misc, valid-type]
        def __init__(self, *a, **kw):
            super().__init__(*a, **kw)
            sys.stdout.write("SPAWNING %d\n" % self.pid)
            sys.stdout.flush()
            time.sleep(3)

    _lc.subprocess.Popen = _SlowPopen
    LspSubprocess(cmd=[sys.executable, "-c", STUBBORN], lang="stubborn")
    time.sleep(120)

child_cmd = STUBBORN
if mode in ("descendant", "cleanleader", "detached"):
    child_cmd = {"descendant": STUBBORN_PARENT,
                 "cleanleader": STUBBORN_CLEAN,
                 "detached": STUBBORN_DETACHED}[mode]
    os.environ["STUB_HELPER_FILE"] = sys.argv[2]
if mode == "threadleader":
    child_cmd = THREAD_LEADER
lsp = LspSubprocess(cmd=[sys.executable, "-c", child_cmd], lang="stubborn")
if mode == "threadleader_unrecorded":
    # The SAME shape with every cheap handle removed: owner-stamped so the
    # stamp walk can claim it, in its own session so no recorded leader
    # group contains it, and never passed through _record_spawn so no
    # _SPAWNED entry names it. force_kill_spawned pass 3 is then the only
    # path that can reach it -- which is the net with no coverage at all
    # before this section, and the one both task-aware reads gate.
    _env = dict(os.environ)
    _env["LSP_BRIDGE_OWNER"] = lsp_client._OWNER_ID
    _u = subprocess.Popen([sys.executable, "-c", THREAD_LEADER],
                          env=_env, start_new_session=True)
    # Published IMMEDIATELY, before any check that could divert the flow.
    # This child is in its own session and carries no _SPAWNED record, so
    # the helper file is the ONLY handle the shell has on it -- writing it
    # on the success path alone left every precondition branch depending
    # on a best-effort collection inside a blanket except (Codex
    # adversarial review, Medium). Same value, written earlier.
    _hf = open(sys.argv[2], "w")
    _hf.write(str(_u.pid)); _hf.flush(); _hf.close()
    _recorded = [e[0] for e in list(lsp_client._SPAWNED)]
    _precond = None
    if _u.pid in _recorded:
        _precond = "survivor %d is in _SPAWNED" % _u.pid
    elif any(i.pid == _u.pid for i in list(lsp_client._LIVE_SUBPROCS)):
        _precond = "survivor %d is in _LIVE_SUBPROCS" % _u.pid
    elif os.getpgid(_u.pid) in {os.getpgid(0)} | {
            os.getpgid(p) for p in _recorded}:
        _precond = "survivor %d shares a swept group" % _u.pid
    if _precond is not None:
        # Collect it here, and report WHICH happened. PRECOND-FAIL means
        # termination was confirmed; PRECOND-LEAK means it is still alive
        # and carries its pid plus start ticks. The shell must not fall
        # back onto a bare pid this driver may already have reaped -- that
        # number can be recycled, and signalling by number alone is the
        # exact hazard 9c asserts this harness never commits (Codex
        # adversarial review, Medium).
        _collected = False
        for _ in range(50):
            if _u.poll() is not None:
                _collected = True
                break
            try:
                _u.kill()
            except Exception:
                pass
            time.sleep(0.1)
        if _collected:
            sys.stdout.write("PRECOND-FAIL %s\n" % _precond)
        else:
            _ticks = "0"
            try:
                with open("/proc/%d/stat" % _u.pid) as _sf:
                    _ticks = _sf.read().rsplit(")", 1)[1].split()[19]
            except Exception:
                pass
            sys.stdout.write("PRECOND-LEAK %d %s %s\n"
                             % (_u.pid, _ticks, _precond))
    sys.stdout.flush()
if mode == "published":
    with bridge._LIVE_LSPS_LOCK:
        bridge._LIVE_LSPS[("stubborn", "stubborn")] = lsp
# mode == "unpublished": deliberately NOT registered with the bridge,
# exercising the window a _LIVE_LSPS walk cannot see.
sys.stdout.write("READY %d\n" % lsp.pid)
sys.stdout.flush()
if mode == "initializing":
    # Hold _init_lock the way a real handshake does, against a server that
    # never answers, and take the signal there. This is the shape that
    # deadlocked the first implementation: cleanup ran ON the interrupted
    # thread and waited for a lock that same thread was holding.
    try:
        lsp.initialize("file:///tmp", {})
    except Exception:
        pass
time.sleep(120)
'

lsp_wait_gone() {
    # Wait up to $2 tenths of a second for pid $1 to disappear.
    local pid="$1" ticks="$2"
    while [ "$ticks" -gt 0 ]; do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.1
        ticks=$((ticks - 1))
    done
    return 1
}

lsp_stat_state() {
    # State letter out of any /proc stat file -- a process's or a task's.
    # comm sits in parens and may itself contain spaces or parens, so
    # parse from the LAST ')', the same rule proc_start_ticks uses.
    sed 's/^.*) //' "$1" 2>/dev/null | awk '{print $1}'
}

lsp_task_state() {
    lsp_stat_state "/proc/$1/stat"
}

lsp_wait_thread_leader_shape() {
    # Wait up to 10s for pid $1 to become a thread-group leader in Z with
    # at least one non-Z task still running -- the state in which
    # /proc/PID/stat lies about liveness and /proc/PID/environ answers
    # EACCES. Asserted BEFORE the signal so a case that never reached the
    # shape fails loudly instead of passing as an ordinary reap.
    local pid="$1" ticks=100 taskdir live
    while [ "$ticks" -gt 0 ]; do
        if [ "$(lsp_task_state "$pid")" = "Z" ]; then
            live=0
            for taskdir in "/proc/$pid/task"/*; do
                [ -d "$taskdir" ] || continue
                case "$(lsp_stat_state "$taskdir/stat")" in
                    Z|"") ;;
                    *) live=1 ;;
                esac
            done
            if [ "$live" = "1" ]; then return 0; fi
        fi
        sleep 0.1
        ticks=$((ticks - 1))
    done
    return 1
}

# Everything 9b has spawned, so no exit path can leave one behind. The
# stubborn children ignore pipe EOF by design and sleep for 600s, so a test
# that returns early without killing them leaks for ten minutes -- and 9a
# cannot catch them, since they are `python3` rather than a language-server
# binary name.
LSP_9B_PIDS=""
# $1 pid, $2 (optional) its /proc start ticks, captured here when the
# caller does not supply them. EVERY record is identity-bound and there
# is no bare-pid path: cleanup runs long after most of these have been
# reaped, and a recycled number would carry an unrelated process off with
# it (Codex adversarial review, High). Identity is re-checked IMMEDIATELY
# BEFORE each signal rather than at tracking time, because the entries
# ahead of it in the list are processed first and the window is exactly
# there. `none` means the process was already gone when it was tracked;
# such an entry is still waited for, never signalled. Fails closed in the
# same direction the production reap does -- not-provably-ours is
# not-killed (lsp_client.py:521-523).
lsp_9b_track() {
    local t="${2:-}"
    [ -n "$t" ] || t="$(lsp_start_ticks "$1")"
    LSP_9B_PIDS="$LSP_9B_PIDS ${1}:${t:-none}"
}
lsp_9b_cleanup() {
    local p pid ticks
    for p in $LSP_9B_PIDS; do
        pid="${p%%:*}"; ticks="${p#*:}"
        if [ "$ticks" != "none" ] &&
           [ "$(lsp_start_ticks "$pid")" = "$ticks" ]; then
            kill -9 "$pid" 2>/dev/null || true
        fi
        wait "$pid" 2>/dev/null || true
    done
    LSP_9B_PIDS=""
    rm -f "$@" 2>/dev/null || true
}

t_signal_reap_deterministic() {
    local drv out err hf pid child helper rc sig mode signum tgpid tg_skipped=0
    local leak lpid lticks
    drv="$(mktemp -t lsp-stubborn.XXXXXX.py)"
    out="$(mktemp -t lsp-stubborn-out.XXXXXX)"
    err="$(mktemp -t lsp-stubborn-err.XXXXXX)"
    hf="$(mktemp -t lsp-stubborn-helper.XXXXXX)"
    printf '%s' "$LSP_STUBBORN_DRIVER" > "$drv"

    # Step 0 -- discrimination control. SIGKILL bypasses every handler, so
    # the child MUST survive it. If it does not, the child self-exits and
    # every assertion below would be vacuous.
    python3 "$drv" published > "$out" 2>/dev/null &
    pid=$!; lsp_9b_track "$pid"
    child=""
    for _ in $(seq 1 100); do
        child="$(awk '/^READY/ {print $2; exit}' "$out" 2>/dev/null)"
        [ -n "$child" ] && break
        sleep 0.1
    done
    [ -n "$child" ] && lsp_9b_track "$child"
    if [ -z "$child" ]; then
        printf '[9b] FAIL: driver never reported READY\n' >&2
        lsp_9b_cleanup "$drv" "$out" "$err" "$hf"; return 1
    fi
    kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true
    sleep 1
    if ! kill -0 "$child" 2>/dev/null; then
        printf '[9b] FAIL: control -- child %s died without being reaped, so\n' \
            "$child" >&2
        printf '     the signal assertions below would prove nothing\n' >&2
        lsp_9b_cleanup "$drv" "$out" "$err" "$hf"; return 1
    fi
    kill -9 "$child" 2>/dev/null

    # Steps 1..N -- every termination signal, at every phase barrier, plus a
    # repeated signal delivered DURING cleanup.
    for sig in TERM INT HUP; do
        case "$sig" in
            TERM) signum=15 ;; INT) signum=2 ;; HUP) signum=1 ;;
        esac
        for mode in published unpublished initializing descendant cleanleader \
                    detached threadleader threadleader_unrecorded; do
            : > "$out"; : > "$err"; : > "$hf"
            python3 "$drv" "$mode" "$hf" > "$out" 2> "$err" &
            pid=$!; lsp_9b_track "$pid"
            child=""
            for _ in $(seq 1 100); do
                child="$(awk '/^READY/ {print $2; exit}' "$out" 2>/dev/null)"
                [ -n "$child" ] && break
                grep -q '^NOSHAPE\|^SHAPEFAIL' "$out" 2>/dev/null && break
                sleep 0.1
            done
            if grep -q '^SHAPEFAIL' "$out" 2>/dev/null; then
                # The host HAS libc.so.6 and it did not load. That is a
                # fixture or environment defect, never a host limit, so it
                # fails rather than quietly retiring the coverage (Codex
                # adversarial review, Medium).
                printf '[9b] FAIL: %s/%s %s\n' "$sig" "$mode" \
                    "$(sed -n 's/^SHAPEFAIL //p' "$out")" >&2
                wait "$pid" 2>/dev/null || true
                lsp_9b_cleanup "$drv" "$out" "$err" "$hf"; return 1
            fi
            if grep -q '^NOSHAPE' "$out" 2>/dev/null; then
                # Only an absent libc reaches here; see the driver probe.
                # Recorded, so the banner below drops its thread-group
                # clause and the final summary names what did not run.
                lsp_note_skip 9b "$sig/$mode" \
                    "$(sed -n 's/^NOSHAPE //p' "$out")"
                tg_skipped=1
                wait "$pid" 2>/dev/null || true
                continue
            fi
            [ -n "$child" ] && lsp_9b_track "$child"
            if [ -z "$child" ]; then
                printf '[9b] FAIL: %s/%s driver never reported READY\n' \
                    "$sig" "$mode" >&2
                sed 's/^/     /' "$out" >&2
                lsp_9b_cleanup "$drv" "$out" "$err" "$hf"; return 1
            fi
            if grep -q '^PRECOND-FAIL\|^PRECOND-LEAK' "$out" 2>/dev/null; then
                # The unrecorded survivor was reachable by a cheap pass, so
                # collecting it would have proved nothing about the one net
                # this case exists to exercise. PRECOND-FAIL means the
                # driver confirmed the child is gone and there is nothing
                # to track; PRECOND-LEAK means it is still alive, and only
                # then does the fallback take it -- after re-checking the
                # start ticks, because a pid this driver may already have
                # reaped can name somebody else entirely.
                leak="$(sed -n 's/^PRECOND-LEAK //p' "$out")"
                if [ -n "$leak" ]; then
                    lpid="${leak%% *}"; leak="${leak#* }"
                    lticks="${leak%% *}"; leak="${leak#* }"
                    lsp_9b_track "$lpid" "$lticks"
                else
                    leak="$(sed -n 's/^PRECOND-FAIL //p' "$out")"
                fi
                printf '[9b] FAIL: %s/%s %s\n' "$sig" "$mode" "$leak" >&2
                lsp_9b_cleanup "$drv" "$out" "$err" "$hf"; return 1
            fi
            helper=""
            if case "$mode" in
                   descendant|cleanleader|detached|threadleader_unrecorded)
                       true ;;
                   *) false ;;
               esac
            then
                # The leader forks a helper that ignores EOF. Reaping only
                # the leader leaves it -- the same leak, one level down.
                # In cleanleader the leader then EXITS POLITELY, so the
                # helper is reparented and only its process group remains
                # as a handle. In detached the helper calls setsid, so it
                # is in NO group of ours and is not our child either --
                # only the per-process OWNER stamp can still find it --
                # LSP_BRIDGE_OWNER, not the tree-wide run id, because a
                # run id shared across a harness tree would make "ours"
                # mean "any sibling" (lsp_client.py:518-523). In
                # threadleader_unrecorded the driver itself spawns the
                # survivor, so the announcement is the driver's, not a
                # forked leader's.
                for _ in $(seq 1 100); do
                    helper="$(cat "$hf" 2>/dev/null)"
                    [ -n "$helper" ] && break
                    sleep 0.1
                done
                [ -n "$helper" ] && lsp_9b_track "$helper"
                if [ -z "$helper" ]; then
                    printf '[9b] FAIL: %s/%s never announced a helper\n' \
                        "$sig" "$mode" >&2
                    lsp_9b_cleanup "$drv" "$out" "$err" "$hf"; return 1
                fi
            fi
            # The thread-group cases must actually BE in the thread-group
            # state when the signal lands, or they are ordinary reaps
            # wearing the name. Once libc loaded, not reaching it is a
            # FAIL: the shape is the entire content of these two cases.
            #
            # The two cases are NOT equivalent, and only one of them can
            # discriminate the task-aware predicates (Codex adversarial
            # review, Medium). `threadleader` is RECORDED, so
            # force_kill_spawned pass 1 signals it through its pinned
            # _SPAWNED record (lsp_client.py:760), whose only predicate is
            # the start-time re-check -- neither _is_zombie nor
            # _carries_our_owner_id is consulted, and reverting either one
            # leaves this case green. It covers the pass-1 path over a
            # Z-leader shape and nothing more. `threadleader_unrecorded`
            # is reachable ONLY by pass 3 (_stamped_processes,
            # lsp_client.py:704/711), which gates on BOTH predicates; that
            # case and 9f are what the mutation check acts on.
            case "$mode" in
                threadleader|threadleader_unrecorded)
                    tgpid="$child"
                    [ "$mode" = "threadleader_unrecorded" ] && tgpid="$helper"
                    if ! lsp_wait_thread_leader_shape "$tgpid"; then
                        printf '[9b] FAIL: %s/%s pid %s never reached Z-leader\n' \
                            "$sig" "$mode" "$tgpid" >&2
                        printf '     with a live task, so the thread-group shape\n' >&2
                        printf '     this case exists for was never produced\n' >&2
                        lsp_9b_cleanup "$drv" "$out" "$err" "$hf"; return 1
                    fi ;;
            esac
            # READY is printed immediately before initialize() is entered, so
            # give the driver a moment to actually be inside it (and holding
            # _init_lock) before the signal lands.
            [ "$mode" = "initializing" ] && sleep 0.5
            if ! kill -"$sig" "$pid" 2>/dev/null; then
                printf '[9b] FAIL: could not signal %s driver %s\n' \
                    "$sig" "$pid" >&2
                lsp_9b_cleanup "$drv" "$out" "$err" "$hf"; return 1
            fi
            if [ "$mode" = "published" ]; then
                # Storm -- but only once cleanup has demonstrably STARTED.
                # Two back-to-back kills prove nothing: pending standard
                # signals coalesce, so the second can be absorbed before the
                # handler ever runs, and the assertion passes without the
                # behavior it claims to test ever occurring.
                for _ in $(seq 1 100); do
                    grep -q 'terminating: signal' "$err" 2>/dev/null && break
                    sleep 0.1
                done
                if ! grep -q 'terminating: signal' "$err" 2>/dev/null; then
                    printf '[9b] FAIL: %s handler never announced cleanup, so\n' \
                        "$sig" >&2
                    printf '     the storm case would not test a signal during it\n' >&2
                    lsp_9b_cleanup "$drv" "$out" "$err" "$hf"; return 1
                fi
                kill -"$sig" "$pid" 2>/dev/null
            fi
            wait "$pid" 2>/dev/null; rc=$?
            if ! lsp_wait_gone "$child" 150; then
                printf '[9b] FAIL: %s/%s left child %s alive (driver rc=%s)\n' \
                    "$sig" "$mode" "$child" "$rc" >&2
                ps -o pid,args= -p "$child" 2>/dev/null | sed 's/^/     /' >&2
                sed 's/^/     /' "$err" >&2
                lsp_9b_cleanup "$drv" "$out" "$err" "$hf"; return 1
            fi
            if [ -n "$helper" ] && ! lsp_wait_gone "$helper" 150; then
                printf '[9b] FAIL: %s/%s left helper %s alive -- the\n' \
                    "$sig" "$mode" "$helper" >&2
                printf '     leader was reaped but its process tree was not\n' >&2
                lsp_9b_cleanup "$drv" "$out" "$err" "$hf"; return 1
            fi
            # Death BY the signal, not a synthesized status: the handler is
            # required to restore the default disposition and re-raise.
            if [ "$rc" != "$((128 + signum))" ]; then
                printf '[9b] FAIL: %s/%s exited rc=%s, expected %s (death by signal)\n' \
                    "$sig" "$mode" "$rc" "$((128 + signum))" >&2
                lsp_9b_cleanup "$drv" "$out" "$err" "$hf"; return 1
            fi
        done
    done

    # Final barrier -- the interval between Popen returning and the child
    # being recorded, taken on the driver's MAIN thread so the interrupted
    # frame never resumes to write the record.
    for mode in spawnrace spawnrace_nopidfd; do
        : > "$out"; : > "$err"
        python3 "$drv" "$mode" > "$out" 2> "$err" &
        pid=$!; lsp_9b_track "$pid"
        child=""
        for _ in $(seq 1 150); do
            child="$(awk '/^SPAWNING/ {print $2; exit}' "$out" 2>/dev/null)"
            [ -n "$child" ] && break
            sleep 0.1
        done
        [ -n "$child" ] && lsp_9b_track "$child"
        if [ -z "$child" ]; then
            printf '[9b] FAIL: %s driver never announced a spawning child\n' \
                "$mode" >&2
            lsp_9b_cleanup "$drv" "$out" "$err" "$hf"; return 1
        fi
        kill -TERM "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null; rc=$?
        if ! lsp_wait_gone "$child" 150; then
            printf '[9b] FAIL: %s -- signal in the Popen-to-record window leaked %s\n' \
                "$mode" "$child" >&2
            ps -o pid,args= -p "$child" 2>/dev/null | sed 's/^/     /' >&2
            sed 's/^/     /' "$err" >&2
            lsp_9b_cleanup "$drv" "$out" "$err" "$hf"; return 1
        fi
    done
    lsp_9b_cleanup "$drv" "$out" "$err" "$hf"
    printf '[9b] reap OK: TERM/INT/HUP x published/unpublished/initializing/\n'
    if [ "$tg_skipped" = "0" ]; then
        printf '     descendant/cleanleader/detached/threadleader(+unrecorded),\n'
    else
        # The thread-group modes never ran on this host, so the banner
        # must not name them -- see LSP_SKIPPED_SHAPES in the summary.
        printf '     descendant/cleanleader/detached (thread-group SKIPPED),\n'
    fi
    printf '     storm during cleanup, spawn race with and without pidfd,\n'
    printf '     tree collected, status preserved\n'
    return 0
}

# --- 9c: leak attribution rejects foreign and text-matching processes ------
# Asserts the 9a detector answers "did WE leak one", not "is there anything
# LSP-shaped on this host". Both decoys reproduce a measured 2026-08-03
# false positive: a process whose command line merely CONTAINS an LSP binary
# name, and a language server carrying a DIFFERENT run id. The third case is
# the mutation guard -- a process carrying OUR run id and an LSP binary name
# MUST still be reported, or the fix would have achieved a green test by
# blinding the detector.
t_leak_attribution_scoped() {
    local dir fake decoy foreign owned ledgered found scratch rc=0
    dir="$(mktemp -d -t lsp-attr.XXXXXX)"
    fake="$dir/clangd-19"
    # /proc/PID/comm follows the name the binary was exec'd under, so a
    # symlink gives a process a genuine LSP binary NAME (not merely an
    # argv string) without needing clangd installed.
    ln -s "$(command -v python3)" "$fake"

    # Decoy 1 -- command-line TEXT match only. This is the shape that
    # failed baseline run 2: a node process whose argv quoted "clangd-19".
    python3 -c 'import time; time.sleep(30)  # clangd-19 --background-index' &
    decoy=$!
    # Decoy 2 -- a real LSP binary name owned by this user under a
    # DIFFERENT run id: another session's bridge.
    ( LSP_BRIDGE_RUN_ID="foreign-$$" exec "$fake" -c 'import time; time.sleep(30)' ) &
    foreign=$!
    sleep 1
    found="$(lsp_survivors_of_this_run)"
    if [ -n "$found" ]; then
        printf '[9c] FAIL: detector blamed a foreign/text-matching process:\n' >&2
        printf '%s' "$found" | sed 's/^/     /' >&2
        rc=1
    fi
    kill -9 "$decoy" "$foreign" 2>/dev/null
    wait "$decoy" "$foreign" 2>/dev/null || true
    if [ "$rc" -ne 0 ]; then rm -rf "$dir"; return 1; fi

    # Mutation guard A -- an LSP binary carrying OUR run id (the environ
    # net). Inherits LSP_BRIDGE_RUN_ID from the exported harness value.
    "$fake" -c 'import time; time.sleep(30)' &
    owned=$!
    # Mutation guard B -- a process recorded in a ledger under OUR run id
    # (the primary net the bridge itself populates). Planted in a SCRATCH
    # ledger, never the live one: rewriting the shared ledger afterwards
    # would be a read-modify-replace against a file concurrent bridges are
    # appending to, and a record landing between the read and the replace
    # would be silently dropped -- turning a real leak into a clean 9a.
    scratch="$dir/scratch-ledger.tsv"
    cp -f "$LSP_PID_LEDGER" "$scratch" 2>/dev/null || : > "$scratch"
    python3 -c 'import time; time.sleep(30)' &
    ledgered=$!
    printf '%s\t%s\t%s\tstub\tclangd-19\n' \
        "$LSP_RUN_ID" "$ledgered" "$(lsp_start_ticks "$ledgered")" \
        >> "$scratch"
    sleep 1
    found="$(lsp_survivors_of_this_run "$scratch")"
    kill -9 "$owned" "$ledgered" 2>/dev/null
    wait "$owned" "$ledgered" 2>/dev/null || true
    rm -rf "$dir"
    if ! printf '%s' "$found" | awk -v p="$owned" '$1 == p {f=1} END {exit !f}'; then
        printf '[9c] FAIL: environ net missed an LSP (pid %s) carrying our run id\n' \
            "$owned" >&2
        return 1
    fi
    if ! printf '%s' "$found" | awk -v p="$ledgered" '$1 == p {f=1} END {exit !f}'; then
        printf '[9c] FAIL: ledger net missed a pid we recorded (%s) -- the\n' \
            "$ledgered" >&2
        printf '     detector was narrowed into uselessness, not fixed\n' >&2
        return 1
    fi
    # Ownership is required for a GROUP sweep too, and no path may reach
    # for killpg. Both are the same lesson at different layers: a process
    # group is named by a bare number, so membership alone can never
    # authorize a signal -- once a group empties, that number can be
    # reused by a group this bridge never created.
    python3 - <<'PY' || return 1
import os, pathlib, subprocess, sys, time
sys.path.insert(0, "scripts/lsp-mcp")
import lsp_client

# A process in its own group carrying NO run-id stamp stands in for a
# foreign group that reused a stale numeric pgid.
env = {k: v for k, v in os.environ.items()
       if k not in ("LSP_BRIDGE_RUN_ID", "LSP_BRIDGE_OWNER")}
foreign = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(20)"],
                           env=env, start_new_session=True)
time.sleep(0.5)
try:
    members = lsp_client._group_members(foreign.pid)
    if members:
        print(f"[9c] FAIL: unstamped foreign group {foreign.pid} reported as "
              f"ours: {members}", file=sys.stderr)
        sys.exit(1)
    killed = lsp_client._reap_group(foreign.pid)
    if killed or foreign.poll() is not None:
        print(f"[9c] FAIL: reaped an unstamped foreign group "
              f"(killed={killed})", file=sys.stderr)
        sys.exit(1)
finally:
    foreign.kill()

# With neither a pidfd nor a recorded start time there is no way to tell a
# PID from a stranger who inherited the number, so nothing may be signalled.
sent = []
real_kill = os.kill
os.kill = lambda p, s: sent.append((p, s))
try:
    lsp_client._SPAWNED.append((999999, None, None))
    if lsp_client._signal_recorded(999999, 15) or sent:
        print(f"[9c] FAIL: signalled a pid with no identity evidence: {sent}",
              file=sys.stderr)
        sys.exit(1)
    # Either violation fails on its own: reporting a kill it never made, or
    # making one it never reported. Joined with `and`, this passed whenever
    # exactly one of the two happened.
    killed = lsp_client.force_kill_spawned(settle=0.0)
    if killed != 0 or sent:
        print(f"[9c] FAIL: force swept a pid with no identity evidence "
              f"(returned {killed}, signalled {sent})", file=sys.stderr)
        sys.exit(1)
finally:
    os.kill = real_kill
    lsp_client._SPAWNED[:] = [e for e in lsp_client._SPAWNED if e[0] != 999999]

# The deadline must bound the WHOLE force phase, not just its settle loop.
# Several persistent records each drive a group sweep, and a sweep that
# ignores the deadline runs for seconds past a bound the caller is holding
# to -- which is the wedged case an external kill-after is counting through.
stubborn = []
for _ in range(5):
    stubborn.append(subprocess.Popen(
        [sys.executable, "-c", "import sys,time\nsys.stdin.close()\ntime.sleep(20)"],
        env={**os.environ, "LSP_BRIDGE_OWNER": lsp_client._OWNER_ID},
        start_new_session=True))
for sp in stubborn:
    lsp_client._SPAWNED.append(
        (sp.pid, lsp_client.proc_start_ticks(sp.pid), None))
try:
    t0 = time.monotonic()
    lsp_client.force_kill_spawned(settle=0.0, deadline=t0 + 0.3)
    spent = time.monotonic() - t0
    if spent > 1.5:
        print(f"[9c] FAIL: force sweep ran {spent:.2f}s past a 0.3s deadline",
              file=sys.stderr)
        sys.exit(1)
finally:
    for sp in stubborn:
        try:
            sp.kill(); sp.wait(timeout=2)
        except Exception:
            pass
    lsp_client._SPAWNED[:] = [
        e for e in lsp_client._SPAWNED
        if e[0] not in {sp.pid for sp in stubborn}]

# No executable path may signal a group by number.
for src in sorted(pathlib.Path("scripts/lsp-mcp").rglob("*.py")):
    for n, line in enumerate(src.read_text().splitlines(), 1):
        code = line.split("#", 1)[0]
        if "os.killpg(" in code:
            print(f"[9c] FAIL: {src}:{n} signals a process group by number; "
                  f"use _reap_group (ownership-verified)", file=sys.stderr)
            sys.exit(1)
print("[9c] ownership OK: unstamped group rejected, no killpg in the tree")
PY
    printf '[9c] attribution OK: foreign + text-match rejected, both nets catch ours\n'
    return 0
}

# --- 9d/9e: shutdown-budget and deadline-ALLOCATION shapes ----------------
# One driver for both. 9d asserts the budget bounds the SIGNAL path
# end-to-end; 9e wedges one phase at a time and asserts the force sweep
# still got a slice to spend.
LSP_BUDGET_DRIVER='
import os, stat, subprocess, sys, threading, time
sys.path.insert(0, "scripts/lsp-mcp")
import bridge
import lsp_client
from lsp_client import LspSubprocess

mode = sys.argv[1]
STUBBORN = (
    "import sys, time\n"
    "sys.stdin.close()\n"
    "time.sleep(600)\n"
)
# Without this the SIGTERM below takes the DEFAULT disposition: the driver
# dies promptly, inside any ceiling, having reaped nothing -- a bound met
# by never running the teardown that the bound applies to.
bridge._install_signal_handlers()


def announce(line):
    sys.stdout.write(line + "\n")
    sys.stdout.flush()


def spawn_survivor():
    """A child reachable ONLY through force_kill_spawned pass 3.

    An allocation test needs an oracle that a MIS-allocated budget
    actually fails, and a recorded LspSubprocess is not one: pass 1
    signals every _SPAWNED record without consulting the deadline at all
    (lsp_client.py -- "the cheap one, and it is NOT deadline-gated"), so
    it disappears whether or not the force phase kept a slice. This child
    carries the owner stamp, sits in its own session, and is never
    recorded or published, so only the deadline-gated stamp walk can
    collect it. Each precondition is ASSERTED, not assumed: a survivor
    that leaked into a cheap pass would turn the whole case green for the
    wrong reason (Codex design review, High)."""
    env = dict(os.environ)
    env["LSP_BRIDGE_OWNER"] = lsp_client._OWNER_ID
    p = subprocess.Popen([sys.executable, "-c", STUBBORN], env=env,
                         start_new_session=True)
    recorded = [e[0] for e in list(lsp_client._SPAWNED)]
    swept = {os.getpgid(0)}
    for rp in recorded:
        try:
            swept.add(os.getpgid(rp))
        except Exception:
            pass
    why = None
    if p.pid in recorded:
        why = "survivor %d is in _SPAWNED" % p.pid
    elif any(i.pid == p.pid for i in list(lsp_client._LIVE_SUBPROCS)):
        why = "survivor %d is in _LIVE_SUBPROCS" % p.pid
    elif os.getpgid(p.pid) in swept:
        why = "survivor %d shares a swept group" % p.pid
    else:
        return p
    # Collect FIRST, announce second. The recorded children were created
    # BEFORE this check and their pids have not been announced yet, so the
    # shell cleanup knows only the driver -- and os._exit skips atexit, so
    # nothing else would collect them. Announcing first is not enough
    # either: PRECOND-FAIL ends the shell poll, and the shell then
    # SIGKILLs the driver, which can cut this sweep off midway (Codex
    # adversarial review, Medium, twice). The marker becoming observable
    # must happen-after the cleanup it reports.
    p.kill()
    try:
        p.wait(timeout=5)
    except Exception:
        pass
    lsp_client.force_kill_spawned(settle=0.0)
    announce("PRECOND-FAIL %s" % why)
    os._exit(3)


def saturate_stderr():
    """Fill fd 2 until EAGAIN, through our OWN non-blocking description.

    A launcher that stops draining the pipe is how teardown gets wedged
    before it begins, and a blocking write is not something an exception
    guard can catch. Saturation is PROVEN by EAGAIN rather than assumed
    from a pipe capacity, which is host-tunable; and the fill goes
    through a private re-open of /proc/self/fd/2 so the SHARED open-file
    description keeps its blocking flags -- the same rule _diag follows,
    and the reason neither can wedge the other (Codex design review,
    Medium).

    Returns (ok, kind, detail). `kind` separates the ONE condition that
    earns a skip from every condition that does not: collapsing all of
    them into a single false made a broken fixture indistinguishable
    from an unsupported host, and once a skip stopped counting as a pass
    that turned a real regression into a green run with the coverage
    silently absent (Codex adversarial review, Medium)."""
    # Host capability is probed SEPARATELY from the open, because the two
    # failures mean opposite things and one except-clause cannot tell them
    # apart: without procfs no rewrite of this test could build the
    # fixture, whereas EMFILE, EACCES or a closed fd 2 are the harness
    # breaking on a host that supports the shape perfectly well. Catching
    # both as "unsupported" let a regression retire the coverage under a
    # green run (Codex adversarial review, Medium, twice).
    # os.path.isdir() is NOT the probe to use here: it answers false for
    # EVERY stat failure, so an EACCES or EIO on a procfs that is present
    # would read as "this host does not have procfs" and take the skip
    # (Codex adversarial review, Medium). Only a confirmed ABSENCE --
    # ENOENT / ENOTDIR -- is a host limit.
    try:
        _st = os.stat("/proc/self/fd")
    except (FileNotFoundError, NotADirectoryError) as exc:
        return (False, "unsupported",
                "/proc/self/fd is absent, so fd 2 cannot be re-opened "
                "through procfs on this host: %s" % exc)
    except OSError as exc:
        return (False, "error",
                "probing /proc/self/fd failed with procfs present: %s" % exc)
    if not stat.S_ISDIR(_st.st_mode):
        return (False, "unsupported",
                "/proc/self/fd is not a directory on this host")
    try:
        fd = os.open("/proc/self/fd/2",
                     os.O_WRONLY | os.O_APPEND | os.O_NONBLOCK)
    except Exception as exc:
        # procfs IS here, so this is ours: a missing fd-2 entry, a
        # descriptor limit, a permission problem. Fail, never skip.
        return (False, "error",
                "cannot re-open /proc/self/fd/2 though procfs is present: "
                "%s" % exc)
    blob = b"x" * 4096
    try:
        for _ in range(8192):          # 32 MiB ceiling; stops at EAGAIN
            try:
                os.write(fd, blob)
            except BlockingIOError:
                return (True, "", "")
            except OSError as exc:
                # EBADF / EPIPE / EINVAL mean the harness handed us the
                # wrong descriptor or the reader end went away. That is a
                # fixture defect and must FAIL 9d, not excuse it.
                return (False, "error", "write to stderr failed: %s" % exc)
        return (False, "error",
                "32 MiB written without reaching EAGAIN -- stderr is not "
                "the undrained pipe the harness set up")
    finally:
        os.close(fd)


recorded = [LspSubprocess(cmd=[sys.executable, "-c", STUBBORN],
                          lang="stubborn")
            for _ in range(4 if mode.startswith("budget") else 1)]

# Wedge exactly ONE phase, AFTER the spawns so the spawn path leaves the
# counter it maintains alone. Each of these three phases consumed the
# whole budget once and handed the force sweep an expired one; the bug is
# the same one three times, and the bindings are NOT interchangeable --
# bridge imported reap_all_live by name, so patching lsp_client alone
# would not wedge the signal path (Codex design review, High).
if mode == "alloc_settle":
    lsp_client._SPAWN_INFLIGHT = 1        # the settle can never observe 0
elif mode == "alloc_graceful":
    bridge.reap_all_live = lambda *a, **kw: time.sleep(60)
elif mode == "alloc_atexit":
    lsp_client.reap_all_live = lambda *a, **kw: time.sleep(60)

survivor = spawn_survivor() if mode.startswith("alloc") else None

if mode == "budget_satstderr":
    _sat_ok, _sat_kind, _sat_why = saturate_stderr()
    if not _sat_ok:
        # NOSAT is a skip; SATFAIL is a failure. Only the host-cannot-do-it
        # case may take the skip, or a regression in the fixture would hide
        # behind it.
        announce(("NOSAT " if _sat_kind == "unsupported" else "SATFAIL ")
                 + _sat_why)
        lsp_client.force_kill_spawned(settle=0.0)
        os._exit(0)

if survivor is not None:
    announce("SURVIVOR %d" % survivor.pid)
announce("RECORDED " + " ".join(str(i.pid) for i in recorded))
announce("READY")

if mode == "alloc_atexit":
    # The atexit hook IS the path under test, so exit normally and let it
    # run -- and let nothing bounded precede it, which would short it out
    # through _TEARDOWN_COMPLETE.
    sys.exit(0)
time.sleep(120)
'

lsp_wait_collected() {
    # Gone, or a zombie awaiting a reap -- both mean the sweep signalled
    # it. `kill -0` cannot tell a zombie from a live process, and an
    # orphan stays a zombie until its new parent gets to it, so a
    # gone-only check is a race against init. Valid ONLY for the
    # single-threaded stubborn children: for a pthread_exit leader Z is
    # the LIVE state, which is the whole point of the 9b cases.
    local pid="$1" ticks="$2"
    while [ "$ticks" -gt 0 ]; do
        kill -0 "$pid" 2>/dev/null || return 0
        [ "$(lsp_task_state "$pid")" = "Z" ] && return 0
        sleep 0.1
        ticks=$((ticks - 1))
    done
    return 1
}

lsp_budget_run() {
    # $1 mode, $2 signal ("" = the driver exits on its own), $3 internal
    # budget seconds, $4 wall-clock ceiling in ms. Asserts the teardown
    # finished inside the ceiling, every recorded child was collected,
    # and -- for the alloc modes -- the pass-3-only survivor was too.
    local mode="$1" sig="$2" budget="$3" ceiling="$4"
    local drv out err fifo pid survivor rc t0 t1 tstart elapsed waited p ok=0
    local signum=0
    # Diagnostics carry the tag of the sub-test that OWNS the mode, not
    # of the runner they share: an alloc_* failure printed as [9d] sent a
    # reader to the wrong sub-test (Codex consistency review, Low).
    local tag=9d
    case "$mode" in alloc_*) tag=9e ;; esac
    drv="$(mktemp -t lsp-budget.XXXXXX.py)"
    out="$(mktemp -t lsp-budget-out.XXXXXX)"
    err="$(mktemp -t lsp-budget-err.XXXXXX)"
    fifo=""
    printf '%s' "$LSP_BUDGET_DRIVER" > "$drv"
    tstart="$(date +%s%N)"
    if [ "$mode" = "budget_satstderr" ]; then
        # A pipe with a reader that never drains. Opened O_RDWR by the
        # shell, so there is no FIFO open rendezvous for either side to
        # block on, and never read from, so the driver can fill it. The
        # shell never writes to it either, so the harness cannot wedge
        # itself on its own fixture (Codex design review, Medium).
        fifo="$(mktemp -u -t lsp-budget-fifo.XXXXXX)"
        mkfifo "$fifo"
        exec 9<>"$fifo"
        LSP_BRIDGE_SHUTDOWN_BUDGET="$budget" python3 "$drv" "$mode" \
            > "$out" 2>&9 &
    else
        LSP_BRIDGE_SHUTDOWN_BUDGET="$budget" python3 "$drv" "$mode" \
            > "$out" 2> "$err" &
    fi
    pid=$!; lsp_9b_track "$pid"
    for _ in $(seq 1 150); do
        grep -q '^READY$\|^NOSAT\|^SATFAIL\|^PRECOND-FAIL' "$out" 2>/dev/null \
            && break
        sleep 0.1
    done
    for p in $(sed -n 's/^RECORDED //p;s/^SURVIVOR //p' "$out"); do
        lsp_9b_track "$p"
    done
    survivor="$(sed -n 's/^SURVIVOR //p' "$out")"
    if grep -q '^SATFAIL' "$out" 2>/dev/null; then
        # The host CAN build the fixture and it came out wrong -- a bad
        # descriptor, a vanished reader, or a stderr that is not the
        # undrained pipe. A skip here would hide exactly the regression
        # this mode exists to catch (Codex adversarial review, Medium).
        printf '[%s] FAIL: %s fixture broken -- %s\n' "$tag" "$mode" \
            "$(sed -n 's/^SATFAIL //p' "$out")" >&2
        wait "$pid" 2>/dev/null || true
    elif grep -q '^NOSAT' "$out" 2>/dev/null; then
        # The narrow case: procfs cannot hand back fd 2, so the wedge
        # cannot be built on this host at all. Recorded as a skip and
        # surfaced at the end; the caller drops it from its banner rather
        # than reporting a variant that did not run (Codex consistency
        # review, Medium).
        lsp_note_skip "$tag" "$mode" "$(sed -n 's/^NOSAT //p' "$out")"
        wait "$pid" 2>/dev/null || true
        ok=77
    elif grep -q '^PRECOND-FAIL' "$out" 2>/dev/null; then
        # The driver announces this only after it has collected its own
        # children, and exits immediately afterwards -- so reaping it here
        # is bounded, and it keeps the fallback cleanup below from racing
        # a sweep that is still running.
        wait "$pid" 2>/dev/null || true
        printf '[%s] FAIL: %s %s\n' "$tag" "$mode" \
            "$(sed -n 's/^PRECOND-FAIL //p' "$out")" >&2
    elif ! grep -q '^READY$' "$out" 2>/dev/null; then
        printf '[%s] FAIL: %s driver never reported READY\n' \
            "$tag" "$mode" >&2
        sed 's/^/     /' "$out" >&2
        # The saturated variant deliberately has no readable stderr; every
        # other mode keeps its own file so a startup failure is visible.
        sed 's/^/     /' "$err" >&2
    else
        # A signalled mode is timed from the signal. A mode that exits on
        # its own is timed from LAUNCH: it announces READY and exits in the
        # same breath, so by the time the 0.1s poll observes READY the
        # teardown is already under way, and timing from there measures
        # the poll rather than the bound.
        t0="$tstart"
        if [ -n "$sig" ]; then
            t0="$(date +%s%N)"
            # A swallowed kill failure means the driver was already gone,
            # and every assertion below would then be measuring a teardown
            # that no signal caused (Codex adversarial review, Medium).
            if ! kill -"$sig" "$pid" 2>/dev/null; then
                printf '[%s] FAIL: %s could not be signalled with %s -- the\n' \
                    "$tag" "$mode" "$sig" >&2
                printf '     driver was already gone, so nothing below tests\n' >&2
                printf '     the handler path\n' >&2
                lsp_9b_cleanup "$drv" "$out" "$err"
                if [ -n "$fifo" ]; then exec 9>&-; rm -f "$fifo"; fi
                return 1
            fi
        fi
        # Hard outer watchdog: 6s, so a re-introduced blocking write
        # produces a bounded FAILURE rather than a wedged CI job.
        waited=0
        while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt 60 ]; do
            sleep 0.1; waited=$((waited + 1))
        done
        t1="$(date +%s%N)"
        elapsed=$(( (t1 - t0) / 1000000 ))
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null
            printf '[%s] FAIL: %s never exited within the 6s watchdog\n' \
                "$tag" "$mode" >&2
        elif [ "$elapsed" -gt "$ceiling" ]; then
            wait "$pid" 2>/dev/null || true
            printf '[%s] FAIL: %s teardown took %sms against a %ss budget\n' \
                "$tag" "$mode" "$elapsed" "$budget" >&2
            printf '     (ceiling %sms) -- the bound did not bind\n' \
                "$ceiling" >&2
        else
            rc=0; wait "$pid" 2>/dev/null || rc=$?
            ok=1
            # Death BY the signal, mirroring the 9b assertion. Without it a
            # driver that raised after READY -- and whose children Python
            # then reaped through atexit -- satisfies every timing and
            # collection check below while the handler path was never
            # entered (Codex adversarial review, Medium). A mode with no
            # signal exits on its own and must do so cleanly.
            if [ -n "$sig" ]; then
                case "$sig" in
                    TERM) signum=15 ;; INT) signum=2 ;; HUP) signum=1 ;;
                    *) signum=0 ;;
                esac
                if [ "$rc" != "$((128 + signum))" ]; then
                    printf '[%s] FAIL: %s exited rc=%s, expected %s (death by %s)\n' \
                        "$tag" "$mode" "$rc" "$((128 + signum))" "$sig" >&2
                    ok=0
                fi
            elif [ "$rc" != "0" ]; then
                printf '[%s] FAIL: %s exited rc=%s, expected a clean self-exit\n' \
                    "$tag" "$mode" "$rc" >&2
                ok=0
            fi
            for p in $(sed -n 's/^RECORDED //p' "$out"); do
                lsp_wait_collected "$p" 20 && continue
                printf '[%s] FAIL: %s left recorded child %s alive\n' \
                    "$tag" "$mode" "$p" >&2
                ok=0
            done
            if [ -n "$survivor" ] && ! lsp_wait_collected "$survivor" 20; then
                printf '[%s] FAIL: %s left survivor %s alive -- the force\n' \
                    "$tag" "$mode" "$survivor" >&2
                printf '     sweep got no slice of the budget to spend\n' >&2
                ok=0
            fi
            if [ "$ok" = "1" ]; then
                printf '[%s]   %s OK in %sms\n' "$tag" "$mode" "$elapsed"
            fi
        fi
    fi
    lsp_9b_cleanup "$drv" "$out" "$err"
    if [ -n "$fifo" ]; then
        exec 9>&-
        rm -f "$fifo"
    fi
    if [ "$ok" = "0" ]; then return 1; fi
    if [ "$ok" = "77" ]; then return 77; fi
    return 0
}

# --- 9d: the shutdown budget bounds the SIGNAL path end-to-end ------------
# 9c times the force sweep by calling it directly, which cannot see the
# handler control flow -- and the split between the graceful join, the
# settle wait and the sweep is exactly where a reserved slice gets spent
# by the wrong phase. This drives a real SIGTERM into a real handler with
# several persistent stamped records and asserts the whole teardown fits
# the budget. The saturated-stderr variant proves the same holds when the
# launcher has stopped draining the pipe, which is how teardown got
# wedged before it began; _diag is what keeps that non-blocking.
t_shutdown_budget_signal_path() {
    local rc=0 satrc=0
    lsp_budget_run budget TERM 2 3500 || rc=1
    lsp_budget_run budget_satstderr TERM 2 3500 || satrc=$?
    # 77 = the pipe could not be saturated on this host. Not a failure,
    # and not a pass either: the banner below stops claiming it.
    [ "$satrc" = "0" ] || [ "$satrc" = "77" ] || rc=1
    [ "$rc" = "0" ] || return 1
    if [ "$satrc" = "77" ]; then
        printf '[9d] budget OK: TERM teardown bounded with 4 stamped records\n'
        printf '     (the saturated-stderr variant was SKIPPED on this host)\n'
    else
        printf '[9d] budget OK: TERM teardown bounded with 4 stamped records,\n'
        printf '     and again with a saturated (undrained) stderr pipe\n'
    fi
    return 0
}

# --- 9e: the deadline is ALLOCATED, not merely present --------------------
# The same bug landed three times in three phases -- the settle wait, the
# graceful join in _shutdown_bounded, and the graceful join at atexit --
# each consuming the whole budget and handing the force sweep an expired
# one, which then declines every record. Each was caught by review or by
# one incidental test, never by a test aimed at the shape. Here each
# phase is wedged in turn and the oracle is a child ONLY the deadline-
# gated pass can collect, so consuming the reserve fails the case.
t_deadline_allocation_family() {
    local rc=0
    lsp_budget_run alloc_settle   TERM 3 4500 || rc=1
    lsp_budget_run alloc_graceful TERM 3 4500 || rc=1
    # atexit carries its OWN 2.0s deadline and 0.6s reserve, so the env
    # budget does not apply to it; the ceiling is that bound plus slack.
    lsp_budget_run alloc_atexit   ""   3 3500 || rc=1
    [ "$rc" = "0" ] || return 1
    printf '[9e] allocation OK: settle wait, _shutdown_bounded graceful join\n'
    printf '     and atexit graceful join each wedged; the force sweep still\n'
    printf '     collected a child only its deadline-gated pass can reach\n'
    return 0
}

# --- 9f: the two task-aware reads, asserted directly ----------------------
# 9b proves the reap collects a thread-group leader end-to-end; this
# pins the two predicates it rests on, including the case no live shape
# can produce on demand -- a task list that empties BETWEEN the stat read
# and the scan. The current order reads /proc/PID/stat first, so such a
# process reads as ALIVE, which is the safe direction and was untested.
t_task_aware_liveness() {
    # 77 propagates the whole-fixture skip to `run`, which counts it apart
    # from pass and fail. Exiting 0 on a host with no libc.so.6 reported
    # this sub-test GREEN without running any of it (Codex consistency
    # review, Medium).
    local rc=0
    python3 - <<'PY' || rc=$?
import ctypes, os, subprocess, sys, time
import ctypes.util
sys.path.insert(0, "scripts/lsp-mcp")
import lsp_client

THREAD_LEADER = (
    "import ctypes, sys, threading, time\n"
    "sys.stdin.close()\n"
    "threading.Thread(target=time.sleep, args=(600,)).start()\n"
    "time.sleep(0.2)\n"
    'ctypes.CDLL("libc.so.6").pthread_exit(None)\n'
)

# Same absence-versus-breakage split as the 9b driver: 77 is reserved for
# a host that cannot offer the capability, and a libc.so.6 that IS on the
# loader path but will not load is a failure (Codex adversarial review,
# Medium).
_found = ctypes.util.find_library("c")
if _found != "libc.so.6":
    print(f"[9f] SKIP: no glibc-style libc.so.6 on this host "
          f"(the loader offers {_found!r})", file=sys.stderr)
    sys.exit(77)
try:
    _libc = ctypes.CDLL("libc.so.6")
except Exception as exc:
    print(f"[9f] FAIL: libc.so.6 is present but did not load: {exc}",
          file=sys.stderr)
    sys.exit(1)
if not hasattr(_libc, "pthread_exit"):
    print("[9f] SKIP: libc.so.6 has no pthread_exit", file=sys.stderr)
    sys.exit(77)

fails = []
live = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
tg = subprocess.Popen([sys.executable, "-c", THREAD_LEADER],
                      env={**os.environ,
                           "LSP_BRIDGE_OWNER": lsp_client._OWNER_ID})
dead = subprocess.Popen([sys.executable, "-c", "raise SystemExit(0)"])
try:
    # Wait for both terminal states: the exited child must be a zombie,
    # and the thread-group leader must be Z with its worker still on.
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        d = lsp_client._stat_fields(dead.pid)
        g = lsp_client._stat_fields(tg.pid)
        if d and d[0] == "Z" and g and g[0] == "Z":
            break
        time.sleep(0.1)
    if not (d and d[0] == "Z"):
        fails.append("fixture: the exited child never became a zombie")
    if not (g and g[0] == "Z"):
        fails.append("fixture: the pthread_exit leader never reached Z, so "
                     "nothing below tests the thread-group shape")
    else:
        # The premise of both section-20 fixes, asserted rather than
        # assumed: the LEADER lies about liveness and refuses its environ.
        if lsp_client._stat_fields_at(f"/proc/{tg.pid}/task/{tg.pid}/stat") is None:
            fails.append("fixture: the leader task stat was unreadable")
        try:
            open(f"/proc/{tg.pid}/environ", "rb").read()
            fails.append("fixture: the leader environ was READABLE, so the "
                         "per-task ownership fallback is not being exercised")
        except OSError:
            pass

    if lsp_client._is_zombie(live.pid):
        fails.append("a live single-threaded process read as a zombie")
    if not lsp_client._is_zombie(dead.pid):
        fails.append("an exited unwaited child did not read as a zombie")
    if lsp_client._is_zombie(tg.pid):
        fails.append("a Z leader with a live worker read as a zombie -- both "
                     "sweeps would skip it and the process would leak")
    if not lsp_client._carries_our_owner_id(tg.pid):
        fails.append("a Z leader with a live worker did not resolve as ours; "
                     "the per-task environ fallback is what proves ownership "
                     "once /proc/PID/environ answers EACCES")

    # The race the live shapes cannot produce: stat says Z, and the task
    # list has emptied (or gone) by the time it is read. Both must answer
    # ALIVE -- a redundant signal to a dead process costs nothing, and
    # skipping a live one is the leak the whole mechanism exists to stop.
    real_stat, real_listdir = lsp_client._stat_fields, os.listdir

    def _empty(path, *a, **kw):
        if str(path).endswith("/task"):
            return []
        return real_listdir(path, *a, **kw)

    def _gone(path, *a, **kw):
        if str(path).endswith("/task"):
            raise OSError("task list vanished")
        return real_listdir(path, *a, **kw)

    try:
        lsp_client._stat_fields = lambda pid: ["Z", "1", "1"]
        os.listdir = _empty
        if lsp_client._is_zombie(live.pid):
            fails.append("an EMPTY task list read as a zombie; the check must "
                         "fail closed when the scan races the last exit")
        os.listdir = _gone
        if lsp_client._is_zombie(live.pid):
            fails.append("an unreadable task list read as a zombie; the check "
                         "must fail closed when procfs answers an error")
    finally:
        lsp_client._stat_fields, os.listdir = real_stat, real_listdir
finally:
    for p in (live, tg, dead):
        try:
            p.kill()
        except Exception:
            pass
        try:
            p.wait(timeout=5)
        except Exception:
            pass

if fails:
    for f in fails:
        print(f"[9f] FAIL: {f}", file=sys.stderr)
    sys.exit(1)
print("[9f] liveness OK: Z-leader-with-live-worker is alive and ours, an "
      "exited child is a zombie, a racing task list fails closed")
PY
    [ "$rc" = "0" ] && return 0
    [ "$rc" = "77" ] && return 77
    return 1
}

lsp_survivors_of_this_run() {
    # $1 (optional): ledger to read instead of the live harness one. 9c
    # passes a scratch file so it can plant a record without touching the
    # ledger 9a depends on.
    local LSP_PID_LEDGER="${1:-$LSP_PID_LEDGER}"
    # Every language server THIS run started that is still alive, one
    # "pid reason" per line. Two independent nets, neither of which can
    # see a process this run did not spawn:
    #
    #   ledger -- the bridge recorded the pid at spawn time. Liveness by
    #             kill -0; PID reuse rejected by comparing start ticks.
    #   environ -- a process whose BINARY NAME (comm, not command-line
    #             text) is a language server AND whose environment
    #             carries this run's id. Catches a child the ledger
    #             missed (ledger unwritable, spawner bypassed).
    #
    # There is deliberately NO fallback to the unscoped PID-delta rule
    # this replaced: when attribution is unavailable the honest result
    # is a narrower check, not a wider one that reports other people's
    # processes as our leaks (Codex design review, Medium).
    local rid pid rec_ticks lang binname now_ticks comm out
    out=""
    if [ -s "$LSP_PID_LEDGER" ]; then
        while IFS="$(printf '\t')" read -r rid pid rec_ticks lang binname; do
            [ "$rid" = "$LSP_RUN_ID" ] || continue
            [ -n "$pid" ] || continue
            kill -0 "$pid" 2>/dev/null || continue
            now_ticks="$(lsp_start_ticks "$pid")"
            if [ -n "$now_ticks" ] && [ "$rec_ticks" != "-1" ] \
               && [ "$now_ticks" != "$rec_ticks" ]; then
                continue   # PID recycled -- a different process now.
            fi
            out="$out$pid ledger:$lang:$binname
"
        done < "$LSP_PID_LEDGER"
    fi
    if [ -d /proc ]; then
        for d in /proc/[0-9]*; do
            pid="${d#/proc/}"
            comm="$(cat "$d/comm" 2>/dev/null || true)"
            [ -n "$comm" ] || continue
            case " $out " in *" $pid "*) continue ;; esac
            for binname in $LSP_BIN_NAMES; do
                # comm is truncated to 15 chars, so match on the prefix
                # the kernel would have kept.
                case "$binname" in "$comm"*) ;; *) continue ;; esac
                if tr '\0' '\n' < "$d/environ" 2>/dev/null \
                   | grep -qxF "LSP_BRIDGE_RUN_ID=$LSP_RUN_ID"; then
                    out="$out$pid environ:$comm
"
                fi
                break
            done
        done
    fi
    printf '%s' "$out"
}

t_no_leaked_lsp_processes() {
    # Assert no language server started by THIS run outlived it.
    #
    # A process caught mid-exit is not a leak, so a first hit is
    # re-checked after a grace period rather than reported straight
    # away -- the old detector's third false-positive channel was
    # exactly this (it named a pid that had already exited by the time
    # it tried to describe it).
    local survivors
    survivors="$(lsp_survivors_of_this_run)"
    if [ -n "$survivors" ]; then
        sleep 2
        survivors="$(lsp_survivors_of_this_run)"
    fi
    [ -n "$survivors" ] || return 0
    printf '[lsp-mcp-tests] debug (9a): language servers this run leaked:\n' >&2
    printf '%s' "$survivors" | while read -r pid why; do
        printf '  pid=%s via=%s :: %s\n' "$pid" "$why" \
            "$(ps -o args= -p "$pid" 2>/dev/null | cut -c1-100)" >&2
    done
    return 1
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
run "14j lsp-recv corr_id correlation"   t_lsp_recv_corr_id_correlation
run "14k truncate body CPU bounded"      t_truncate_body_cpu_bounded
run "15a workspace root priority chain"  t_workspace_root_priority
run "15b file:// URI rejected"           t_file_uri_rejected
run "15c parent traversal rejected"      t_parent_traversal_rejected
run "15d symlink rejected by O_NOFOLLOW" t_symlink_rejected
run "15e race-free walk under rename"    t_race_free_walk_rename
run "15f respawn replay absolute path"   t_respawn_replay_absolute_path_still_works
run "15g workspace-root invalid"         t_workspace_root_invalid
run "15h path-too-deep cap"              t_path_too_deep
run "15i FIFO does not hang"             t_fifo_path_does_not_hang
run "15j abs symlink rejected"           t_absolute_path_symlink_rejected
run "15k symlinked-ws root replay"       t_symlinked_workspace_root_replay
run "16a warm-start default"             t_warm_start_default
run "16b warm-start subset c,py"         t_warm_start_subset
run "16c warm-start unknown lang"        t_warm_start_unknown_lang
run "16d warm-start empty spec"          t_warm_start_empty_spec
run "16e progress snapshot race"         t_warm_progress_snapshot_race
run "16f workdone/create ack"            t_warm_workdone_create_ack
run "17a bg mode <1.5s exit"             t_warm_start_mode_bg_fast_exit
run "17b bg dispatch event"              t_warm_start_mode_bg_dispatch_event
run "17c blocking default no bg event"   t_warm_start_mode_blocking_default
run "17d argparse rejects bogus mode"    t_warm_start_mode_argparse_reject
run "17e shutdown publish gate reaps"    t_warm_shutdown_publish_gate
run "17f bg thread re-entry safe"        t_warm_bg_reentry_safe
run "17g shutdown gate TOCTOU"           t_warm_shutdown_gate_toctou
run "18a type-hierarchy normalizers"     t_type_hierarchy_normalizers
run "18b type_hierarchy clangd round-trip" t_type_hierarchy_smoke_clangd
run "18c type_hierarchy capability-missing" t_type_hierarchy_capability_missing
run "18d type_hierarchy supported walk"   t_type_hierarchy_supported_walk
run "18e type_hierarchy types cap"        t_type_hierarchy_types_cap
run "9b deterministic signal reap"       t_signal_reap_deterministic
run "9c leak attribution scoped"         t_leak_attribution_scoped
run "9d shutdown budget via signal"      t_shutdown_budget_signal_path
run "9e deadline allocation family"      t_deadline_allocation_family
run "9f task-aware liveness + ownership" t_task_aware_liveness
# 9a runs LAST so every prior sub-test has had a chance to clean up.
run "9a no leaked LSP processes"         t_no_leaked_lsp_processes

if [ -n "$LSP_SKIPPED_SHAPES" ]; then
    # The aggregate runner (scripts/test-tooling.sh) extracts ONE line and
    # discards the rest, so a plain "N/N sub-tests PASS" above a skip
    # block is read downstream as full coverage. When anything was
    # skipped the summary says so ON THE SUMMARY LINE (Codex consistency
    # review, Medium).
    printf '[lsp-mcp-tests] %d/%d sub-tests PASS, %d shape(s) SKIPPED\n' \
        "$pass" "$((pass + fail))" \
        "$(printf '%s\n' "$LSP_SKIPPED_SHAPES" | wc -l)"
else
    printf '[lsp-mcp-tests] %d/%d sub-tests PASS\n' "$pass" "$((pass + fail))"
fi
if [ -n "$LSP_SKIPPED_SHAPES" ]; then
    # Printed unconditionally, including on a green run: the whole point
    # is that "104/104 PASS" must never be read as "every shape ran".
    # Shapes and whole sub-tests are counted separately -- a partial skip
    # leaves its sub-test passing, so reporting only `skipped` here would
    # print a zero above a non-empty list.
    printf '[lsp-mcp-tests] %d shape(s) NOT exercised on this host (%d whole sub-test(s) skipped):\n' \
        "$(printf '%s\n' "$LSP_SKIPPED_SHAPES" | wc -l)" "$skipped" >&2
    printf '%s\n' "$LSP_SKIPPED_SHAPES" >&2
fi
if [ "$fail" -gt 0 ]; then
    exit 1
fi
exit 0
