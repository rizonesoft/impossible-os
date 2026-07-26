#!/usr/bin/env bash
# ============================================================================
# scripts/todo-graph/tests/test_query_bounds.sh -- query-surface output bounds.
#
# Covers the bounding layer added to query.py + mcp_server.py:
#   1. default limit applies when --limit is absent
#   2. fail-closed on ceiling breach, INCLUDING the narrowing flags
#   3. deterministic ordering of bounded pages
#   4. envelope fields always present
#   5. the MCP tool surface can never return an unbounded set
#   6. the verbs left unbounded on purpose stay unbounded
#   7. no answers are lost: page/scope/limit-0 reach the unbounded result
#
# Companion to tests/test_build.sh Test 9 (which owns subcommand semantics);
# this file owns only what comes OUT of them.
# ============================================================================
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
QUERY_PY="$REPO_ROOT/scripts/todo-graph/query.py"
BUILD_PY="$REPO_ROOT/scripts/todo-graph/build.py"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

PASS=0
FAIL=0
t_pass() { PASS=$((PASS + 1)); echo "  ok   $1"; }
t_fail() { FAIL=$((FAIL + 1)); echo "  FAIL $1"; }

CACHE="$TMP_DIR/cache.json"
python3 "$BUILD_PY" --quiet --output "$CACHE" >/dev/null 2>&1 \
    || { echo "[test_query_bounds] FATAL: build.py failed priming cache"; exit 2; }

q() { python3 "$QUERY_PY" --cache "$CACHE" --repo-root "$REPO_ROOT" --quiet "$@"; }

echo "== query bounds =="

# --- 1. Default limit applies when the flag is absent ---------------------
DEF_LIMIT=$(python3 -c "
import sys; sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import query; print(query.DEFAULT_ROW_LIMIT)")
RET=$(q --json by-domain | python3 -c "import sys,json; print(json.load(sys.stdin)['returned'])")
TOT=$(q --json by-domain | python3 -c "import sys,json; print(json.load(sys.stdin)['total_matching'])")
if [ "$RET" = "$DEF_LIMIT" ] && [ "$TOT" -gt "$DEF_LIMIT" ]; then
    t_pass "default limit ($DEF_LIMIT) applied when --limit absent (total=$TOT)"
else
    t_fail "default limit not applied (returned=$RET total=$TOT expected returned=$DEF_LIMIT)"
fi

# The default must bound a verb whose worst case is large, not just one.
RET_BL=$(q --json backlinks native-api-ssdt --limit 10 \
    | python3 -c "import sys,json; print(json.load(sys.stdin)['returned'])")
if [ "$RET_BL" = "10" ]; then
    t_pass "explicit --limit honoured on backlinks (returned=10)"
else
    t_fail "explicit --limit ignored on backlinks (returned=$RET_BL)"
fi

# --- 2. Envelope fields always present ------------------------------------
# Including for an empty result set: absence of rows must not mean absence
# of the fields that say whether the set was complete.
for SPEC in "by-domain" "orphans" "stale" "backlinks native-api-ssdt"; do
    # shellcheck disable=SC2086
    if q --json $SPEC | python3 -c "
import sys, json
d = json.load(sys.stdin)
req = {'returned', 'total_matching', 'truncated', 'limit', 'offset'}
missing = req - set(d)
sys.exit(1 if missing or 'rows' not in d else 0)" 2>/dev/null; then
        t_pass "envelope fields present: $SPEC"
    else
        t_fail "envelope fields missing: $SPEC"
    fi
done

EMPTY_OK=$(q --json stale --days 1000000 | python3 -c "
import sys, json
d = json.load(sys.stdin)
print('yes' if d['rows'] == [] and d['returned'] == 0
      and d['truncated'] is False and 'total_matching' in d else 'no')")
if [ "$EMPTY_OK" = "yes" ]; then
    t_pass "envelope present and honest on an empty result"
else
    t_fail "empty result envelope wrong ($EMPTY_OK)"
fi

# --- 3. Fail-closed on ceiling breach + narrowing flags returned ----------
CEIL_OUT=$(q --json by-domain --max-bytes 500 2>/dev/null)
CEIL_RC=$?
if [ "$CEIL_RC" = "3" ]; then
    t_pass "ceiling breach exits 3 (distinct from 2 = bad input)"
else
    t_fail "ceiling breach exit code wrong (got $CEIL_RC, want 3)"
fi
if printf '%s' "$CEIL_OUT" | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert d['error'] == 'output-ceiling-exceeded', d.get('error')
assert d['returned'] == 0, 'must not emit a partial set'
assert d['truncated'] is True
assert isinstance(d['total_matching'], int) and d['total_matching'] > 0
assert d['ceiling_bytes'] == 500
assert d['would_emit_bytes'] > 500
flags = ' '.join(d['narrow_with'])
for want in ('--limit', '--scope', '--fields', '--offset'):
    assert want in flags, f'missing narrowing flag {want}'
" 2>/dev/null; then
    t_pass "ceiling envelope: bound + total_matching + all narrowing flags"
else
    t_fail "ceiling envelope malformed: $CEIL_OUT"
fi

# A silently-truncated set that reads as complete is the failure this
# whole layer exists to prevent -- assert no rows leaked out.
if ! printf '%s' "$CEIL_OUT" | grep -q '"rows"'; then
    t_pass "ceiling breach emits no rows key (cannot read as a complete set)"
else
    t_fail "ceiling breach leaked a rows key"
fi

# --- 4. Deterministic ordering of bounded pages ---------------------------
P1=$(q --json by-domain --limit 20 | python3 -c "
import sys,json; print(','.join(r['id'] for r in json.load(sys.stdin)['rows']))")
P2=$(q --json by-domain --limit 20 | python3 -c "
import sys,json; print(','.join(r['id'] for r in json.load(sys.stdin)['rows']))")
if [ "$P1" = "$P2" ] && [ -n "$P1" ]; then
    t_pass "bounded page is re-runnable (identical across runs)"
else
    t_fail "bounded page not deterministic"
fi

# Rebuilding the cache must not reshuffle a page either.
CACHE2="$TMP_DIR/cache2.json"
python3 "$BUILD_PY" --quiet --output "$CACHE2" >/dev/null 2>&1
P3=$(python3 "$QUERY_PY" --cache "$CACHE2" --repo-root "$REPO_ROOT" --quiet \
    --json by-domain --limit 20 | python3 -c "
import sys,json; print(','.join(r['id'] for r in json.load(sys.stdin)['rows']))")
if [ "$P1" = "$P3" ]; then
    t_pass "bounded page survives a cache rebuild (total ordering)"
else
    t_fail "page reshuffled after cache rebuild"
fi

# Paging must partition the set: page1 + page2 == first 40, no gap/overlap.
PAGED=$(q --json by-domain --limit 20 --offset 20 | python3 -c "
import sys,json; print(','.join(r['id'] for r in json.load(sys.stdin)['rows']))")
FIRST40=$(q --json by-domain --limit 40 | python3 -c "
import sys,json; print(','.join(r['id'] for r in json.load(sys.stdin)['rows']))")
if [ "$P1,$PAGED" = "$FIRST40" ]; then
    t_pass "offset paging partitions cleanly (no gap, no overlap)"
else
    t_fail "offset paging inconsistent with a single larger page"
fi

# --- 5. No answers lost ---------------------------------------------------
# The bounded default plus its stated narrowing path must reach the same
# set the unbounded call returns.
FULL=$(q --json by-domain --limit 0 | python3 -c "
import sys,json; print(','.join(sorted(r['id'] for r in json.load(sys.stdin)['rows'])))")
STITCHED=$(python3 -c "
import json, subprocess, sys
ids = []
off = 0
while True:
    out = subprocess.run([sys.executable, '$QUERY_PY', '--cache', '$CACHE',
                          '--repo-root', '$REPO_ROOT', '--quiet', '--json',
                          'by-domain', '--limit', '50', '--offset', str(off)],
                         capture_output=True, text=True).stdout
    d = json.loads(out)
    ids += [r['id'] for r in d['rows']]
    off += 50
    if off >= d['total_matching']:
        break
print(','.join(sorted(ids)))")
if [ "$FULL" = "$STITCHED" ] && [ -n "$FULL" ]; then
    t_pass "paging reconstructs the complete set exactly (no answer lost)"
else
    t_fail "paged reconstruction != unbounded set"
fi

# Scope narrowing must be a subset of, and consistent with, the full set.
SCOPED=$(q --json by-domain --scope 02-kernel-core --limit 0 | python3 -c "
import sys, json
d = json.load(sys.stdin)
print(d['total_matching'], all(r['domain'] == '02-kernel-core' for r in d['rows']))")
if [ "${SCOPED##* }" = "True" ] && [ "${SCOPED%% *}" -gt 0 ]; then
    t_pass "--scope narrows to exactly one domain (${SCOPED%% *} rows)"
else
    t_fail "--scope filter wrong ($SCOPED)"
fi

# --limit 0 is the terminating path of the no-lost-answers guarantee, so
# the byte ceiling must NOT apply to it. Regression: applying the ceiling
# here made the complete set unreachable in JSON entirely.
LIM0=$(q --json stale --days 0 --limit 0 2>/dev/null)
LIM0_RC=$?
if [ "$LIM0_RC" = "0" ] && printf '%s' "$LIM0" | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert 'error' not in d, d.get('error')
assert d['returned'] == d['total_matching'], 'complete set not returned'
assert d['truncated'] is False
assert len(json.dumps(d)) > $(python3 -c "
import sys; sys.path.insert(0,'$REPO_ROOT/scripts/todo-graph')
import query; print(query.OUTPUT_CEILING_BYTES)"), 'payload not actually over ceiling'
" 2>/dev/null; then
    t_pass "--limit 0 bypasses the ceiling (complete set reachable)"
else
    t_fail "--limit 0 blocked by ceiling: complete set unreachable (rc=$LIM0_RC)"
fi

# A bound that hides a blocker is worse than no bound. The stamp verbs
# sort severity-first so a truncated page cannot drop a Critical/High
# while keeping a Medium. Regression: a severity-blind sort lost 2 of 6
# Criticals from `deferred native-api-ssdt` at the default limit.
if python3 - <<PY
import json, subprocess, sys, collections
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import query
def call(*a):
    return json.loads(subprocess.run(
        [sys.executable, "$QUERY_PY", "--cache", "$CACHE",
         "--repo-root", "$REPO_ROOT", "--quiet", "--json", *a],
        capture_output=True, text=True).stdout)
BLOCKER = {"critical", "h", "high"}
bad = []
for verb in ("deferred", "deferred-by"):
    full = call(verb, "native-api-ssdt", "--limit", "0")
    dflt = call(verb, "native-api-ssdt")
    fs = collections.Counter(str(r.get("severity") or "").lower() for r in full["rows"])
    ds = collections.Counter(str(r.get("severity") or "").lower() for r in dflt["rows"])
    for sev in fs:
        if sev in BLOCKER and fs[sev] > ds.get(sev, 0):
            bad.append(f"{verb}: lost {fs[sev]-ds.get(sev,0)} {sev!r}")
    # severity rank must be non-decreasing down the page
    ranks = [query._severity_rank(r.get("severity")) for r in dflt["rows"]]
    if ranks != sorted(ranks):
        bad.append(f"{verb}: page not severity-ordered")
sys.exit(1 if bad else 0)
PY
then
    t_pass "blocker-class rows survive truncation (severity-first ordering)"
else
    t_fail "a bounded page dropped a Critical/High row"
fi

# --- 6. Unsupported narrowing fails closed rather than being ignored ------
if q --json deferred native-api-ssdt --scope 02-kernel-core >/dev/null 2>&1; then
    t_fail "--scope on a domain-less verb should fail, not be ignored"
else
    t_pass "--scope on a domain-less verb fails closed"
fi
if q --json by-domain --fields not_a_column >/dev/null 2>&1; then
    t_fail "unknown --fields should fail, not be silently dropped"
else
    t_pass "unknown --fields fails closed"
fi

# --- 7. Field projection actually shrinks the payload ---------------------
WIDE=$(q --json by-domain --limit 50 | wc -c)
NARROW=$(q --json by-domain --limit 50 --fields domain,id | wc -c)
if [ "$NARROW" -lt "$WIDE" ]; then
    t_pass "--fields projection shrinks payload ($WIDE -> $NARROW bytes)"
else
    t_fail "--fields did not shrink payload ($WIDE -> $NARROW)"
fi

# --- 8. Deliberately-unbounded verbs stay unbounded -----------------------
# stats is O(taxonomy); it must keep its top-level keys (no envelope) so
# existing consumers and test_build.sh 9l keep working.
if q --json stats | python3 -c "
import sys, json
d = json.load(sys.stdin)
req = {'total_nodes','by_status','by_domain','top_blocking',
       'top_longest_deferred','avg_dep_depth','orphan_count'}
sys.exit(0 if req <= set(d) and 'rows' not in d else 1)" 2>/dev/null; then
    t_pass "stats stays unwrapped and unbounded (no envelope imposed)"
else
    t_fail "stats shape changed"
fi

# --- 9. MCP surface can never return an unbounded set ---------------------
if timeout 300 python3 - <<PY
import sys, json
sys.path.insert(0, "$REPO_ROOT/scripts/todo-graph")
import mcp_server as M, query as Q
from pathlib import Path
repo = Path("$REPO_ROOT")

# Every clamp input, including query.py's unlimited sentinel, must land
# inside [1, MCP_MAX_ROW_LIMIT].
for raw in (None, 0, -1, -99, "0", "abc", 1, 50, 10**9):
    v = M._clamp_limit(raw)
    assert 1 <= v <= Q.MCP_MAX_ROW_LIMIT, f"clamp escaped: {raw!r} -> {v}"
assert M._clamp_limit(0) != Q.UNLIMITED_ROW_LIMIT, "unlimited reachable via 0"

# Bounded tools must never return more rows than the cap, whatever is asked.
for tool, args in (("by-domain", {"limit": 10**9}),
                   ("by-domain", {"limit": 0}),
                   ("orphans", {"limit": -1}),
                   ("stale", {"days": 0, "limit": 10**9}),
                   ("backlinks", {"target": "native-api-ssdt", "limit": 10**9})):
    d = json.loads(M._dispatch_tool(tool, args, repo, auto_rebuild=False))
    assert "error" not in d, f"{tool} {args} -> {d.get('error')}"
    assert d["returned"] <= Q.MCP_MAX_ROW_LIMIT, f"{tool} returned {d['returned']}"
    for k in ("returned", "total_matching", "truncated"):
        assert k in d, f"{tool} missing envelope field {k}"

# Every bounded tool must advertise limit in its MCP schema, and stats
# must not (it has no rows to page).
for name in M.MCP_TOOLS:
    props = M._tool_schema(name)["properties"]
    if name in Q.UNBOUNDED_SUBCOMMANDS:
        assert "limit" not in props, f"{name} should not advertise limit"
    else:
        assert "limit" in props, f"{name} missing limit in schema"
        assert props["limit"]["maximum"] == Q.MCP_MAX_ROW_LIMIT
print("mcp-ok")
PY
then
    t_pass "MCP surface bounded: clamp, row cap, envelope, schema"
else
    t_fail "MCP surface can return an unbounded set"
fi

echo
echo "== test_query_bounds: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ] || exit 1
