#!/usr/bin/env bash
# ============================================================================
# test_build_sections_a.sh -- todo-graph build suite shard: Tests 7 and 23, the producer window, sections 24, 27 and 29.
#
# One of four shards scripts/test-tooling.sh starts together; the shared
# preamble, helpers and shard list are in test_build_lib.sh.
# ============================================================================

set -u
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/test_build_lib.sh" || exit 2

# The blocks here ran after Test 12's `cd "$REPO_ROOT"` in the single file,
# so they start where they always did.
cd "$REPO_ROOT" || exit 2

# Test 7: performance budget (build CPU time per MiB of corpus; wall-clock is reported).
# ----------------------------------------------------------------------
# The budget is the build's CPU TIME (user + system, including the subprocesses
# it waits for), not wall-clock. Wall-clock measured the HOST: the same bytes
# took 560, 1445 and 1490 ms by hand and 2057-3397 ms inside a loaded pre-push
# pack (2026-09-03/04), refusing pushes of green trees. CPU time excludes time
# spent descheduled, so a busy machine no longer fails a fast build, while a
# build that really does 2 s of work still fails. The wall time is reported too.
# The budget is a RATE, not a flat number (2026-09-28 close-out): the build is
# linear in corpus bytes (fence_scan dominates the profile), and the flat 2 s sat
# on top of the real cost once todo/ reached ~13.5 MB -- 1595-2069 ms CPU,
# 118-153 ms/MiB measured. 250 ms/MiB is ~1.6x over the worst reading, so a build
# whose per-byte work doubles still fails; 2000 ms stays the floor.
BUILD_MS_PER_MB=250
CORPUS_KB=$(( $(find "$REPO_ROOT/todo" -name '*.md' -type f -print0 | xargs -0 cat | wc -c) / 1024 ))
BUILD_BUDGET_MS=$(( CORPUS_KB * BUILD_MS_PER_MB / 1024 ))
[ "$BUILD_BUDGET_MS" -ge 2000 ] || BUILD_BUDGET_MS=2000
BUILD_CPU_MS=$(python3 - "$BUILD_PY" "$TMP_DIR/cache-perf.json" <<'PYCPU'
import resource, subprocess, sys
r0 = resource.getrusage(resource.RUSAGE_CHILDREN)
subprocess.run([sys.executable, sys.argv[1], "--quiet", "--output", sys.argv[2]],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
r1 = resource.getrusage(resource.RUSAGE_CHILDREN)
print(int(round(((r1.ru_utime - r0.ru_utime) + (r1.ru_stime - r0.ru_stime)) * 1000)))
PYCPU
)
START_NS=$(mono_ns)
python3 "$BUILD_PY" --quiet --output "$TMP_DIR/cache-perf.json" >/dev/null 2>&1
END_NS=$(mono_ns)
# A NON-POSITIVE elapsed is an invalid MEASUREMENT, not a fast build, and the
# naive `-lt 2000` banked it as a pass -- so a clock step turned this budget
# into a guaranteed green. Observed three times on 2026-08-06 under WSL2
# (`-1158ms` in this very test, plus `-262ms`/`-246ms` in hand timings), where
# the host clock resyncs after a suspend. Refuse the reading instead of
# trusting it: a budget that cannot fail is not a budget.
#
# The SOURCE is now monotonic (mono_ns), so this refusal is unreachable in
# practice rather than removed -- which is the point. If it ever fires again
# the measurement really is broken, and the right response is to diagnose it,
# not to re-run until it passes.
if ! ELAPSED_MS=$(elapsed_ms "$START_NS" "$END_NS"); then
    t_fail "build timing unreadable (start='$START_NS' end='$END_NS') -- monotonic clock source failed"
else
    if [ "$ELAPSED_MS" -le 0 ]; then
        t_fail "build timing invalid (${ELAPSED_MS}ms) -- the monotonic clock did not advance"
    elif ! [ "${BUILD_CPU_MS:-x}" -ge 0 ] 2>/dev/null; then
        t_fail "build CPU time unreadable ('${BUILD_CPU_MS}') -- getrusage measurement failed"
    elif [ "$BUILD_CPU_MS" -lt "$BUILD_BUDGET_MS" ]; then
        t_pass "build within CPU budget (${BUILD_CPU_MS}ms CPU of ${BUILD_BUDGET_MS}ms for ${CORPUS_KB} KiB, ${ELAPSED_MS}ms wall)"
    else
        t_fail "build exceeded CPU budget: ${BUILD_CPU_MS}ms CPU > ${BUILD_BUDGET_MS}ms (${BUILD_MS_PER_MB} ms/MiB x ${CORPUS_KB} KiB of todo/, floor 2000ms; ${ELAPSED_MS}ms wall)"
    fi
fi

# ----------------------------------------------------------------------
# Tests 7a-7c: the monotonic clock source itself.
#
# Test 7 above, the star-traversal ceiling, and the teardown budgets in
# scripts/test-tooling.sh + scripts/lsp-mcp/tests/test_bridge.sh all now rest
# on mono_ns/valid_ns, so those two helpers are load-bearing and get the same
# treatment as any other gate here: prove they FAIL on the shapes they exist to
# catch, not merely that they pass on a good reading.
# ----------------------------------------------------------------------

# 7a: mono_ns yields an all-digits reading that never goes backwards.
MONO_A=$(mono_ns); MONO_B=$(mono_ns)
if valid_ns "$MONO_A" && valid_ns "$MONO_B" && [ "$MONO_B" -ge "$MONO_A" ]; then
    t_pass "mono_ns: readings are all-digits and non-decreasing"
else
    t_fail "mono_ns: bad readings a='$MONO_A' b='$MONO_B'"
fi

# 7b: elapsed_ms REFUSES every malformed reading, on either side, and returns a
# correct delta on good ones. Each shape below is a distinct hazard, not a
# variation on one: `2000+1` evaluates to a plausible POSITIVE number under
# unguarded arithmetic and would sail through a non-positive refusal; `foo`
# aborts the whole suite under `set -u`; `08` is invalid octal to bash and kills
# the caller mid-function; a 19-digit value is past INT64 and wraps silently.
EM_BAD=0
for em_shape in '' '2000+1' 'foo' '-5' '1.5' '12 34' ' 12' $'12\n34' '08' '0008' '9223372036854775808' '99999999999999999999'; do
    if EM_OUT=$(elapsed_ms "$em_shape" 2000000000) 2>/dev/null; then
        t_fail "elapsed_ms: accepted malformed START '$em_shape' (returned '$EM_OUT')"
        EM_BAD=1
    elif [ -n "${EM_OUT:-}" ]; then
        t_fail "elapsed_ms: refused START '$em_shape' but still printed '$EM_OUT'"
        EM_BAD=1
    fi
    if EM_OUT=$(elapsed_ms 1000000000 "$em_shape") 2>/dev/null; then
        t_fail "elapsed_ms: accepted malformed END '$em_shape' (returned '$EM_OUT')"
        EM_BAD=1
    fi
done
if [ "$EM_BAD" = "0" ] && [ "$(elapsed_ms 1000000000 3500000000)" = "2500" ] \
   && [ "$(elapsed_ms 0 0)" = "0" ]; then
    t_pass "elapsed_ms: refuses expression, identifier, octal, overflow and multi-token readings; correct on good ones"
else
    t_fail "elapsed_ms: guard/delta check failed (bad=$EM_BAD, 1s-3.5s=$(elapsed_ms 1000000000 3500000000 || echo REFUSED))"
fi

# 7c: the mutation that proves 7b is worth running -- the SAME expression-shaped
# string, fed to bash arithmetic UNGUARDED (which is what every one of these
# sites did before), evaluates to a positive number and would therefore pass a
# `-le 0` refusal, while elapsed_ms refuses it AND the caller pattern lands in
# its failure branch instead of aborting. If this stops holding, 7b is guarding
# a hazard bash no longer has.
EM_MUT="2000+1"
EM_BRANCH=none
if ! EM_MS=$(elapsed_ms 1000000000 "$EM_MUT"); then EM_BRANCH=refused; else EM_BRANCH="measured:$EM_MS"; fi
if [ "$(( EM_MUT ))" -gt 0 ] && [ "$EM_BRANCH" = "refused" ]; then
    t_pass "elapsed_ms: mutation check -- unguarded arithmetic banks '$EM_MUT' as $(( EM_MUT )), the caller takes the refusal branch"
else
    t_fail "elapsed_ms: mutation check did not flip (arith=$(( EM_MUT )), branch=$EM_BRANCH)"
fi

# 7d: the WIRING pin. 7b/7c prove the helper is sound, which is not the same as
# proving the timing sites USE it -- delete any single call-site guard and both
# stay green (Codex adversarial review, Medium). So assert the property
# directly, across all three harnesses: a variable assigned from `$(mono_ns)`
# must never appear inside an arithmetic expansion. elapsed_ms takes its
# readings as ARGUMENTS, so a correctly-wired file has zero hits, and reverting
# one site to a raw `$(( end - start ))` fires this immediately.
# The todo-graph build suite is ONE harness in five files (the lib and its four
# shards), so its readings are collected across all of them
# and every file is scanned: a shard added later is covered by the glob, and a
# reading taken in one file and subtracted in another is still caught.
EM_WIRE="$(python3 - "$REPO_ROOT" <<'PY'
import glob, os, re, sys
root = sys.argv[1]
tb = sorted(os.path.relpath(p, root) for p in
            glob.glob(f"{root}/scripts/todo-graph/tests/test_build*.sh"))
harnesses = [("test_build", tb),
             ("scripts/test-tooling.sh", ["scripts/test-tooling.sh"]),
             ("scripts/lsp-mcp/tests/test_bridge.sh", ["scripts/lsp-mcp/tests/test_bridge.sh"])]
bad, wired = [], []
for label, rels in harnesses:
    texts = {}
    for rel in rels:
        try:
            texts[rel] = open(f"{root}/{rel}", encoding="utf-8").read()
        except OSError as e:
            bad.append(f"{rel}: unreadable ({e})")
    if not texts:
        bad.append(f"{label}: no files found")
        continue
    whole = "\n".join(texts.values())
    names = set(re.findall(r'(\w+)="?\$\(mono_ns\)', whole))
    if not names:
        bad.append(f"{label}: no mono_ns readings found (did the helper get renamed?)")
        continue
    if "elapsed_ms " not in whole:
        bad.append(f"{label}: reads mono_ns but never calls elapsed_ms")
    for rel, text in texts.items():
        for lineno, line in enumerate(text.splitlines(), 1):
            if line.lstrip().startswith("#"):
                continue
            for expr in re.findall(r'\$\(\((.*?)\)\)', line):
                hit = [n for n in names if re.search(rf'\b{re.escape(n)}\b', expr)]
                if hit:
                    bad.append(f"{rel}:{lineno}: raw arithmetic on mono_ns reading(s) "
                               f"{sorted(hit)} -- route it through elapsed_ms")
    wired.append(f"{label}({len(texts)} files, {len(names)})")
print("OK " + " ".join(wired) if not bad else "BAD " + " | ".join(bad))
PY
)"
case "$EM_WIRE" in
    OK\ *) t_pass "elapsed_ms: every mono_ns reading in all three harnesses is routed through it (${EM_WIRE#OK })" ;;
    *)     t_fail "elapsed_ms: wiring pin -- ${EM_WIRE#BAD }" ;;
esac

# 7e: the DRIFT pin. 7b/7c run the vector against THIS file's helpers and 7d
# proves the other two harnesses call theirs -- neither notices if a copy's
# BEHAVIOUR diverges. The helpers are duplicated per script on purpose (three
# standalone harnesses, no shared sourcing), so flipping `0*) return 1` to
# `return 0` in one copy silently restores the octal-abort hazard and every
# test above stays green (Codex consistency audit, Medium).
#
# So run the malformed-input vector against ALL THREE COPIES and require an
# identical verdict from each. The copies are extracted textually and sourced
# in a subshell -- these harnesses cannot be sourced whole, because doing so
# would execute the entire suite.
EM_HDIR="$TMP_DIR/helper-copies"
mkdir -p "$EM_HDIR"
EM_EXTRACT="$(python3 - "$REPO_ROOT" "$EM_HDIR" <<'PY'
import re, sys
root, outdir = sys.argv[1], sys.argv[2]
files = {"tb": "scripts/todo-graph/tests/test_build_lib.sh",
         "tt": "scripts/test-tooling.sh",
         "br": "scripts/lsp-mcp/tests/test_bridge.sh"}
missing = []
for tag, rel in files.items():
    text = open(f"{root}/{rel}", encoding="utf-8").read()
    body = []
    for fn in ("mono_ns", "valid_ns", "elapsed_ms"):
        m = re.search(rf'^{fn}\(\) \{{\n(?:.*?\n)*?\}}$', text, re.M)
        if not m:
            missing.append(f"{rel}: {fn}() not found at column 0")
            continue
        body.append(m.group(0))
    with open(f"{outdir}/{tag}.sh", "w", encoding="utf-8") as f:
        f.write("\n".join(body) + "\n")
print("OK" if not missing else "BAD " + " | ".join(missing))
PY
)"
if [ "$EM_EXTRACT" != "OK" ]; then
    t_fail "elapsed_ms: drift pin could not extract the helper copies -- ${EM_EXTRACT#BAD }"
else
    # em_probe <helpers-file> -- a one-line behavioural signature of one copy.
    #
    # Each probe records the RETURN (A:<value> accepted / R refused) and, on a
    # separate axis, whether the call wrote to STDERR (`+E`). The second axis is
    # load-bearing: this probe runs without `set -e`, so a copy that wrongly
    # ACCEPTS `08` does not abort here the way it would in the real harness --
    # bash reports the invalid-octal error and the function returns non-zero, so
    # a status-only signature would read the hazard as a clean refusal and the
    # drift would be invisible. The stderr text itself is not compared, only its
    # presence, so the signature does not depend on bash's wording.
    em_probe() (
        # shellcheck disable=SC1090
        . "$1"
        errf="$EM_HDIR/probe.err"
        sig=""
        for p in '' '2000+1' 'foo' '-5' '1.5' '08' '0008' '9223372036854775808' '0' '7'; do
            : > "$errf"
            if o=$(elapsed_ms "$p" 3000000000 2>"$errf"); then tok="A:$o"; else tok="R"; fi
            [ -s "$errf" ] && tok="$tok+E"
            sig="$sig|$tok"
        done
        printf '%s\n' "${sig}|D:$(elapsed_ms 1000000000 3500000000 2>/dev/null || echo REFUSED)"
    )
    EM_SIG_TB=$(em_probe "$EM_HDIR/tb.sh")
    EM_SIG_TT=$(em_probe "$EM_HDIR/tt.sh")
    EM_SIG_BR=$(em_probe "$EM_HDIR/br.sh")
    # The signature the vector MUST produce: every malformed shape refused, the
    # two canonical readings accepted, the good delta exact. Pinned literally so
    # a drift that happens to hit all three copies at once is caught too.
    EM_SIG_WANT="|R|R|R|R|R|R|R|R|A:3000|A:2999|D:2500"
    # Mutation: drift ONE copy's leading-zero branch, exactly as a careless edit
    # would, and require the signature to change. Without this, 7e could be
    # comparing three copies of a probe that tests nothing.
    sed 's/^        0\*) return 1 ;;$/        0*) return 0 ;;/' \
        "$EM_HDIR/tt.sh" > "$EM_HDIR/tt-drifted.sh"
    EM_SIG_MUT=$(em_probe "$EM_HDIR/tt-drifted.sh")
    if [ "$EM_SIG_TB" != "$EM_SIG_WANT" ]; then
        t_fail "elapsed_ms: drift pin -- test_build_lib.sh copy: got '$EM_SIG_TB' want '$EM_SIG_WANT'"
    elif [ "$EM_SIG_TT" != "$EM_SIG_WANT" ]; then
        t_fail "elapsed_ms: drift pin -- test-tooling.sh copy diverged: got '$EM_SIG_TT' want '$EM_SIG_WANT'"
    elif [ "$EM_SIG_BR" != "$EM_SIG_WANT" ]; then
        t_fail "elapsed_ms: drift pin -- test_bridge.sh copy diverged: got '$EM_SIG_BR' want '$EM_SIG_WANT'"
    elif [ "$EM_SIG_MUT" = "$EM_SIG_WANT" ]; then
        t_fail "elapsed_ms: drift pin did not flip on a drifted copy -- the probe proves nothing"
    else
        t_pass "elapsed_ms: all three harness copies behave identically, and a drifted copy is detected"
    fi
fi

# ----------------------------------------------------------------------
# Test 23: ONE SHARED CACHE-SCHEMA VALIDATOR FOR BOTH CACHE READERS (s17)
#
# The two readers used to disagree about which caches are valid at all, so the
# SAME bad cache produced an infrastructure refusal on one side and a clean
# lint on the other. Every fixture below is therefore asserted against BOTH
# readers in the SAME sub-test: a rule fixed on one side and not the other
# fails here instead of passing twice.
#
# THE ORACLE IS NOT "THE TWO READERS AGREE". Two readers sharing one module
# agree by construction, including when the module is wrong -- that is a
# correlated failure, not a verification (Codex design review, section 17).
# Each SCHEMA fixture is therefore ALSO checked against
# scripts/todo-graph/schema/cache.schema.json via `jsonschema`, an oracle
# derived from the checked-in contract rather than from our own code.
# ----------------------------------------------------------------------
CS_TREE="$TMP_DIR/cs-tree"
mkdir -p "$CS_TREE/todo/01-test" "$CS_TREE/build" "$CS_TREE/src"
cat > "$CS_TREE/todo/01-test/TODO-01-cs.md" <<'MD'
# TODO-01 cache-schema fixture
MD
printf 'void f(void) { return; }\n' > "$CS_TREE/src/a.c"

# The GOOD cache: schema-valid, non-empty population. Every mutation below is
# this document with exactly one thing changed, so a fixture cannot fail for an
# unrelated reason.
python3 - "$CS_TREE/build/good.json" <<'PY'
import json, sys
json.dump([{"file_path": "todo/01-test/TODO-01-cs.md",
            "stamped_items": [{"section_n": 1, "item_idx": 0,
                               "item_text": "shipped a thing",
                               "refs": [{"kind": "symbol", "file": "src/a.c",
                                         "symbol": "f"}]}]}],
          open(sys.argv[1], "w"))
PY
# The cache must carry a corpus binding or the shared freshness rule fires.
bind_cache "$CS_TREE/build/good.json" "$CS_TREE/todo"

# cs_case <label> <mutation-python> <want_lint_rc> <want_snap_rc> <schema_verdict>
#   schema_verdict: reject | accept | skip  (the INDEPENDENT jsonschema oracle)
cs_case() {
    local label="$1" mut="$2" want_lint="$3" want_snap="$4" schema_verdict="$5"
    local f="$CS_TREE/build/case.json"
    python3 - "$CS_TREE/build/good.json" "$f" <<PY
import json, sys
doc = json.load(open(sys.argv[1]))
$mut
json.dump(doc, open(sys.argv[2], "w"))
PY
    bind_cache "$f" "$CS_TREE/todo"
    # Reader B: lint Check 7 helper.
    STUB_LINT_CACHE="$f" STUB_LINT_REPO_ROOT="$CS_TREE" \
        STUB_LINT_ALLOW_NO_BASELINE=1 \
        python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
        >"$TMP_DIR/cs-lint.out" 2>"$TMP_DIR/cs-lint.err"
    local lint_rc=$?
    # Reader A: corpus resolution snapshot.
    STUB_LINT_CACHE="$f" STUB_LINT_REPO_ROOT="$CS_TREE" \
        python3 "$REPO_ROOT/scripts/todo-graph/corpus_resolution_snapshot.py" \
        write "$TMP_DIR/cs-snap.json" >"$TMP_DIR/cs-snap.err" 2>&1
    local snap_rc=$?
    if [ "$lint_rc" != "$want_lint" ] || [ "$snap_rc" != "$want_snap" ]; then
        t_fail "shared cache schema: $label -- lint rc=$lint_rc (want $want_lint), snapshot rc=$snap_rc (want $want_snap)"
        return
    fi
    if [ "$schema_verdict" != "skip" ]; then
        CS_SCHEMA_VERDICT="$schema_verdict" python3 - "$f" \
            "$REPO_ROOT/scripts/todo-graph/schema/cache.schema.json" \
            >"$TMP_DIR/cs-schema.err" 2>&1 <<'PY'
import json, os, sys
try:
    import jsonschema
except ImportError:
    print("FAIL: jsonschema not installed -- run scripts/setup-deps.sh")
    sys.exit(2)
doc = json.load(open(sys.argv[1]))
schema = json.load(open(sys.argv[2]))
want = os.environ["CS_SCHEMA_VERDICT"]
# SCOPE THE ORACLE TO WHAT THE MODULE CLAIMS. cache_schema validates the
# `stamped_items` SUBTREE, not the node-level frontmatter (build.py owns that),
# so validating a fixture node against the whole node schema would reject it
# for missing `id`/`domain`/`created_at` -- an unrelated reason that would make
# every verdict here meaningless. Pull the subschema out of the same checked-in
# file so the oracle is still derived from the contract, not from our code.
#
# EVERY SUBTREE THE MODULE VALIDATES, not just the section-17 one. Scoping the
# oracle to `stamped_items` alone left the section-19 subtrees with no
# independent check at all, so a Python rule with no schema counterpart (or the
# reverse) could not be detected -- the drift this oracle exists to catch
# (Codex consistency, section 19 review, [medium]).
SUBTREES = ("stamped_items", "sections", "stamps_xrefs")
try:
    if not doc:
        # No nodes at all: nothing to scope to, so check the ROOT shape. This
        # is the case that documents the deliberate divergence -- the schema
        # permits `[]` (it describes what the producer may emit) while both
        # readers refuse it (they additionally need something to walk).
        jsonschema.validate(doc, schema)
    else:
        for node in doc:
            for name in SUBTREES:
                if name in node:
                    jsonschema.validate(
                        node[name], schema["items"]["properties"][name])
    got = "accept"
except jsonschema.ValidationError:
    got = "reject"
if got != want:
    print(f"FAIL: independent schema oracle said {got}, expected {want}")
    sys.exit(1)
PY
        if [ $? -ne 0 ]; then
            t_fail "shared cache schema: $label -- $(cat "$TMP_DIR/cs-schema.err")"
            return
        fi
    fi
    t_pass "shared cache schema: $label"
}

# 23a: BASELINE -- the good cache passes BOTH readers. Without this every
# rejection below could be passing for an unrelated reason (the mutation-check).
cs_case "valid cache passes both readers" "pass" 0 0 accept

# 23b/23c: THE FALSEY SHAPES -- the defect that motivated the section. These are
# wrong-typed but FALSEY, so the lint's `... or []` / `.get("refs", [])` treated
# them as ABSENT and walked ZERO refs while reporting a clean run.
cs_case "falsey stamped_items {} is rejected, not silently skipped" \
    'doc[0]["stamped_items"] = {}' 5 3 reject
cs_case "falsey refs {} is rejected, not silently skipped" \
    'doc[0]["stamped_items"][0]["refs"] = {}' 5 3 reject

# 23d: `section_n` is a DICT KEY in both walks, so a non-integer is unhashable
# or mis-keys the section scope.
cs_case "string section_n is rejected (schema says integer)" \
    'doc[0]["stamped_items"][0]["section_n"] = "1"' 5 3 reject

# 23e: required fields. `_load_nodes` permitted these to be absent; the
# checked-in schema does not.
cs_case "missing required item_text is rejected" \
    'del doc[0]["stamped_items"][0]["item_text"]' 5 3 reject

# 23f: `kind` is a schema `const`, not free text -- the readers branch on it, so
# an unknown kind drops the ref from every bucket rather than being counted.
cs_case "unknown ref kind is rejected" \
    'doc[0]["stamped_items"][0]["refs"][0]["kind"] = "wat"' 5 3 reject

# 23g: additionalProperties false -- an unknown key is producer drift, and
# failing closed forces schema and readers to move together.
cs_case "unknown ref key is rejected (additionalProperties false)" \
    'doc[0]["stamped_items"][0]["refs"][0]["extra"] = 1' 5 3 reject

# 23g2/23g3: `file_path` IS a consumed node field -- both walks build occurrence
# keys from `node.get("file_path") or "?"`. Unvalidated, a list-valued one is
# TRUTHY and gets stringified into the key, so the snapshot reports a bogus
# DROPPED/ADDED at rc 1 (a VERDICT) or writes a poisoned baseline, instead of
# refusing at rc 3. The schema oracle is skipped here: `file_path` lives on the
# node, outside the `stamped_items` subschema this oracle is scoped to.
cs_case "wrong-typed file_path is an infrastructure error, not a verdict" \
    'doc[0]["file_path"] = ["a", "b"]' 5 3 skip
cs_case "missing file_path is an infrastructure error, not a verdict" \
    'del doc[0]["file_path"]' 5 3 skip

# 23h: EMPTY ARRAY is a READINESS refusal, and the one place the readers
# DELIBERATELY diverge from the schema: cache.schema.json permits `[]` because
# it describes what the producer may legally EMIT, while a reader additionally
# needs something to walk. The oracle is asserted to ACCEPT here on purpose --
# that documents the divergence instead of hiding it.
cs_case "empty node array is refused by both readers" \
    'doc.clear()' 5 3 accept

# 23i: LEGACY -- the field absent from EVERY node is a pre-extension cache, a
# genuine "run build". It is the ONE cache-level condition the lint still routes
# to its WARN code (3), and it must stay distinguishable from 23j below.
cs_case "legacy cache with no stamped_items key is the WARN case" \
    'del doc[0]["stamped_items"]' 3 3 accept

# 23j: THE [high] DESIGN FINDING. A cache that HAS the field but carries an
# EMPTY population is a producer regression, NOT an old cache. The two readers
# take deliberately OPPOSITE actions on this single fact, which is why the
# shared module reports the population rather than ruling on it:
#   snapshot -> rc 3, because a snapshot over nothing is a vacuous baseline
#   lint     -> does NOT short-circuit; the walk proceeds so the tool's own
#               rc 7 population gate can adjudicate the loss. Pre-empting it
#               with a cache-level rc 3 would route a REGRESSION through
#               lint.sh as a WARNING -- and warnings leave the overall lint
#               exit at 0, so the population loss would ship.
# Here the floor is bypassed, so "not pre-empted" shows up as rc 0; 23k proves
# the gate it hands off to actually fires.
cs_case "empty population: snapshot refuses, lint defers to its population gate" \
    'doc[0]["stamped_items"] = []' 0 3 accept

# 23k: ...and the hand-off is real. With a baseline recording a population of 1,
# the same emptied cache must reach rc 7 (POPULATION SHRANK), the ERROR path.
# This is the assertion that would have failed under the rejected design.
cat > "$CS_TREE/build/case.json" <<'JSON'
[{"file_path": "todo/01-test/TODO-01-cs.md", "stamped_items": []}]
JSON
bind_cache "$CS_TREE/build/case.json" "$CS_TREE/todo"
mkdir -p "$CS_TREE/scripts/lint"
cat > "$CS_TREE/scripts/lint/stub-lint-baseline.json" <<'JSON'
{"resolved": 1, "total": 1}
JSON
STUB_LINT_CACHE="$CS_TREE/build/case.json" STUB_LINT_REPO_ROOT="$CS_TREE" \
    python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
    >"$TMP_DIR/cs-pop.out" 2>"$TMP_DIR/cs-pop.err"
CS_POP_RC=$?
if [ "$CS_POP_RC" = "7" ] && grep -q "POPULATION" "$TMP_DIR/cs-pop.err"; then
    t_pass "shared cache schema: emptied population reaches the rc 7 gate (not a WARN)"
else
    t_fail "shared cache schema: emptied population gave rc=$CS_POP_RC (want 7 + POPULATION); $(head -2 "$TMP_DIR/cs-pop.err")"
fi

# 23k2: A ZERO BASELINE MUST NOT DISARM THE POPULATION GATE. 23k proves the
# hand-off fires against a positive floor; this proves it cannot be silenced by
# re-recording the baseline at zero while the producer is collapsed. Without
# the `<= 0` rule, occurrences=0 vs total=0 compares equal, the resolved floor
# 0 >= 0 passes, and the run exits 0 with lint.sh rendering `resolved=0/0` as
# full-coverage INFO -- the collapse made permanent and invisible.
cat > "$CS_TREE/scripts/lint/stub-lint-baseline.json" <<'JSON'
{"resolved": 0, "total": 0}
JSON
STUB_LINT_CACHE="$CS_TREE/build/case.json" STUB_LINT_REPO_ROOT="$CS_TREE" \
    python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
    >"$TMP_DIR/cs-zero.out" 2>"$TMP_DIR/cs-zero.err"
CS_ZERO_RC=$?
if [ "$CS_ZERO_RC" = "8" ]; then
    t_pass "shared cache schema: a zero baseline is rejected, not a free pass"
else
    t_fail "shared cache schema: zero baseline gave rc=$CS_ZERO_RC (want 8); $(head -2 "$TMP_DIR/cs-zero.err")"
fi
# Restore the positive floor so later fixtures are unaffected.
cat > "$CS_TREE/scripts/lint/stub-lint-baseline.json" <<'JSON'
{"resolved": 1, "total": 1}
JSON

# 23k3: THE CACHE BYTE CEILING is enforced BEFORE the file is materialised, so
# a hostile cache reports a documented reason instead of an OOM kill outside
# the exit-code contract. Asserted by shrinking the ceiling rather than writing
# a 64 MiB fixture.
CS_BIG_RC=$(STUB_LINT_CACHE="$CS_TREE/build/good.json" STUB_LINT_REPO_ROOT="$CS_TREE" \
    python3 -c "
import sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
cs._MAX_CACHE_BYTES = 4
try:
    cs.load_and_validate('$CS_TREE/build/good.json', '$CS_TREE/todo')
    print('NORAISE')
except cs.CacheSchemaError as e:
    print(e.reason)
" 2>&1)
if [ "$CS_BIG_RC" = "UNREADABLE" ]; then
    t_pass "shared cache schema: an oversized cache is a documented reason, not an OOM"
else
    t_fail "shared cache schema: oversized cache gave '$CS_BIG_RC' (want UNREADABLE)"
fi

# 23k4: FRESHNESS IS BOUND TO THE BYTES PARSED. The corpus binding is located
# by the sha256 of the bytes the caller ACTUALLY READ, never by a fresh read of
# the pathname -- `build.py` rewrites that exact path. Passing bytes that are
# not the file's must therefore refuse: there is no binding for a generation
# nobody published. Without that, a concurrent rebuild lets a reader parse the
# OLD cache and have the NEW file's binding certify it.
CS_TOCTOU=$(python3 -c "
import sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
try:
    cs.check_freshness('$CS_TREE/build/good.json', '$CS_TREE/todo',
                       cache_bytes=b'[] # not the bytes on disk')
    print('NORAISE')
except cs.CacheSchemaError as e:
    print(e.reason)
" 2>&1)
if [ "$CS_TOCTOU" = "STALE" ]; then
    t_pass "shared cache schema: freshness binds to the bytes read, not the pathname"
else
    t_fail "shared cache schema: bound-bytes freshness gave '$CS_TOCTOU' (want STALE)"
fi

# 23k4b: AND THE HAPPY PATH IS REAL. A fixture that refuses for the right
# reason proves nothing if it also refuses for every other reason, so assert
# that the SAME cache with its own bytes and its own binding is accepted.
CS_FRESH=$(python3 -c "
import pathlib, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
p = pathlib.Path('$CS_TREE/build/good.json')
try:
    cs.check_freshness(p, '$CS_TREE/todo', cache_bytes=p.read_bytes())
    print('FRESH')
except cs.CacheSchemaError as e:
    print(e.reason, e)
" 2>&1)
if [ "$CS_FRESH" = "FRESH" ]; then
    t_pass "shared cache schema: a correctly bound cache is accepted"
else
    t_fail "shared cache schema: bound cache gave '$CS_FRESH' (want FRESH)"
fi

# 23k4c: A MISSING BINDING IS STALE, NOT FRESH. A cache copied without its
# sidecar, or one produced before section 21, carries no statement about the
# corpus it was built from -- and "no evidence" is a rebuild condition, not a
# pass. This is the fail-closed direction the pre-section-17 lint got wrong
# three separate ways.
CS_UNBOUND=$(python3 -c "
import pathlib, shutil, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
src = pathlib.Path('$CS_TREE/build/good.json')
dst = pathlib.Path('$CS_TREE/build/unbound.json')
shutil.copyfile(src, dst)          # the cache, without its binding
try:
    cs.check_freshness(dst, '$CS_TREE/todo', cache_bytes=dst.read_bytes())
    print('NORAISE')
except cs.CacheSchemaError as e:
    print(e.reason)
" 2>&1)
if [ "$CS_UNBOUND" = "STALE" ]; then
    t_pass "shared cache schema: a cache with no corpus binding is refused"
else
    t_fail "shared cache schema: unbound cache gave '$CS_UNBOUND' (want STALE)"
fi

# 23k5: A MemoryError ANYWHERE IN THE LOAD is a documented reason, not rc 1.
# The read, the decode and validate_nodes all allocate outside the json.loads
# handler, and both readers catch only CacheSchemaError -- so an OOM in any
# other stage escaped as a bare rc 1, which the snapshot DOCUMENTS as "a prior
# verdict changed". Fault-injected at the read, which is the stage furthest
# from the original handler.
CS_OOM=$(python3 -c "
import builtins, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
real_open = builtins.open
class BoomFile:
    def __init__(self, fh): self._fh = fh
    def fileno(self): return self._fh.fileno()
    def read(self, *a): raise MemoryError('injected')
    def close(self): return self._fh.close()
builtins.open = lambda *a, **k: (BoomFile(real_open(*a, **k))
    if str(a[0]).endswith('good.json') else real_open(*a, **k))
try:
    cs.load_and_validate('$CS_TREE/build/good.json', '$CS_TREE/todo')
    print('NORAISE')
except cs.CacheSchemaError as e:
    print(e.reason)
except MemoryError:
    print('ESCAPED')
" 2>&1)
if [ "$CS_OOM" = "UNREADABLE" ]; then
    t_pass "shared cache schema: a MemoryError in the read is a reason, not a bare rc 1"
else
    t_fail "shared cache schema: injected MemoryError gave '$CS_OOM' (want UNREADABLE)"
fi

# 23k6: THE TODO CORPUS IS GENERATION-BOUND. The freshness scan is not
# instantaneous -- it hashes the corpus file by file -- so a TODO that changed
# before the scan reaches it must be SEEN, or a reader certifies a cache against
# a corpus it no longer matches. (A TODO that changes AFTER it is hashed is
# caught by the post-walk re-verification in 23k7, which is the window that
# actually matters.) Injected by rewriting the file the scan is about to hash,
# from inside that hash -- which is order-independent, unlike picking a "later"
# file out of an os.walk whose order is not defined.
CS_GEN=$(python3 -c "
import pathlib, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
real = cs.file_digest
state = {'victim': None, 'original': None}
def hooked(path):
    if state['victim'] is None:
        state['victim'] = pathlib.Path(path)
        state['original'] = state['victim'].read_text()
        state['victim'].write_text(state['original'] + '\nchanged before the scan reached it\n')
    return real(path)
cs.file_digest = hooked
try:
    p = pathlib.Path('$CS_TREE/build/good.json')
    cs.check_freshness(p, '$CS_TREE/todo', cache_bytes=p.read_bytes())
    print('NORAISE' if state['victim'] else 'NOTINJECTED')
except cs.CacheSchemaError as e:
    print(e.reason if state['victim'] else 'NOTINJECTED')
finally:
    cs.file_digest = real
    if state['victim'] is not None:
        state['victim'].write_text(state['original'])
" 2>&1)
if [ "$CS_GEN" = "STALE" ]; then
    t_pass "shared cache schema: a TODO changing mid-scan refuses instead of certifying"
else
    t_fail "shared cache schema: mid-scan corpus mutation gave '$CS_GEN' (want STALE)"
fi

# 23k7: THE POST-WALK CORPUS RE-VERIFICATION. `check_freshness` proves the cache
# matches the corpus at ONE instant; the window that matters is the caller's ~1s
# resolution walk after it. check_corpus_unchanged is what callers run once that
# walk finishes, so a TODO edited DURING the walk yields an infrastructure
# refusal instead of a verdict over stale nodes.
# THE MUTATION IS A CONTENT EDIT, not an mtime bump: the fingerprint is a
# content digest since section 21, so back-dating or touching a file is
# correctly NOT a change and would leave this fixture asserting nothing. The
# TODO stays edited; the bind_cache calls below reissue the bindings for it.
CS_POST=$(python3 -c "
import pathlib, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
todo = pathlib.Path('$CS_TREE/todo/01-test/TODO-01-cs.md')
corpus = cs._scan_corpus('$CS_TREE/todo', lambda e: None)
# The caller's walk happens here; a TODO changes during it.
todo.write_text(todo.read_text() + '\nedited during the walk\n')
try:
    cs.check_corpus_unchanged('$CS_TREE/todo', corpus)
    print('NORAISE')
except cs.CacheSchemaError as e:
    print(e.reason)
" 2>&1)
if [ "$CS_POST" = "STALE" ]; then
    t_pass "shared cache schema: a TODO edited DURING the walk refuses post-walk"
else
    t_fail "shared cache schema: post-walk corpus check gave '$CS_POST' (want STALE)"
fi
# A None fingerprint (check_stale=False caller) must be a no-op, not a crash.
CS_POST_NONE=$(python3 -c "
import sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
cs.check_corpus_unchanged('$CS_TREE/todo', None)
print('NOOP')
" 2>&1)
if [ "$CS_POST_NONE" = "NOOP" ]; then
    t_pass "shared cache schema: post-walk check is a no-op without a fingerprint"
else
    t_fail "shared cache schema: None fingerprint gave '$CS_POST_NONE' (want NOOP)"
fi
bind_cache "$CS_TREE/build/good.json" "$CS_TREE/todo"
bind_cache "$CS_TREE/build/case.json" "$CS_TREE/todo"

# 23k8: A RAISING close() must not escape, and must not MASK an in-flight
# CacheSchemaError. Bare fh.close() in the finally sat outside every OSError
# normalization path, so a close failure escaped as a raw OSError -- the lint
# exits an undocumented rc 1 and the snapshot exits its DOCUMENTED "a prior
# verdict changed" code (Codex adversarial, section 17 review).
CS_CLOSE=$(python3 -c "
import builtins, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
real_open = builtins.open
class BadClose:
    def __init__(self, fh): self._fh = fh
    def fileno(self): return self._fh.fileno()
    def read(self, *a): return self._fh.read(*a)
    def close(self):
        self._fh.close(); raise OSError('injected close failure')
builtins.open = lambda *a, **k: (BadClose(real_open(*a, **k))
    if str(a[0]).endswith('good.json') else real_open(*a, **k))
try:
    cs.load_and_validate('$CS_TREE/build/good.json', '$CS_TREE/todo')
    print('NORAISE')
except cs.CacheSchemaError as e:
    print(e.reason)
except OSError:
    print('ESCAPED')
" 2>&1)
if [ "$CS_CLOSE" = "UNREADABLE" ]; then
    t_pass "shared cache schema: a failing close is a reason, not a raw OSError"
else
    t_fail "shared cache schema: failing close gave '$CS_CLOSE' (want UNREADABLE)"
fi

# 23k8b: ...and the same holds when the load runs INSIDE an unrelated outer
# except block. The first version discriminated with sys.exc_info(), which is
# AMBIENT -- it also reports an exception being handled by an outer caller, so a
# perfectly successful load called from someone else's handler looked like it
# was already failing and its close error was swallowed, silently certifying a
# load whose descriptor never closed (Codex re-adversarial, section 17).
CS_CLOSE_OUTER=$(python3 -c "
import builtins, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
real_open = builtins.open
class BadClose:
    def __init__(self, fh): self._fh = fh
    def fileno(self): return self._fh.fileno()
    def read(self, *a): return self._fh.read(*a)
    def close(self):
        self._fh.close(); raise OSError('injected close failure')
builtins.open = lambda *a, **k: (BadClose(real_open(*a, **k))
    if str(a[0]).endswith('good.json') else real_open(*a, **k))
try:
    raise ValueError('an unrelated outer failure')
except ValueError:
    # The load SUCCEEDS here; only close() fails. Ambient exception state must
    # not make that look like an already-failing body.
    try:
        cs.load_and_validate('$CS_TREE/build/good.json', '$CS_TREE/todo')
        print('SWALLOWED')
    except cs.CacheSchemaError as e:
        print(e.reason)
    except OSError:
        print('ESCAPED')
" 2>&1)
if [ "$CS_CLOSE_OUTER" = "UNREADABLE" ]; then
    t_pass "shared cache schema: close failure still reported inside an outer except"
else
    t_fail "shared cache schema: close-in-outer-except gave '$CS_CLOSE_OUTER' (want UNREADABLE)"
fi

# 23k9: A ZERO-RESOLVED baseline is as vacuous as a zero total. With every
# symbol in an unresolved bucket and {"resolved":0,"total":N}, the population
# comparison matches and 0 < 0 is false, so the walk exited 0 having examined
# no function body at all.
cat > "$CS_TREE/scripts/lint/stub-lint-baseline.json" <<'JSON'
{"resolved": 0, "total": 1}
JSON
STUB_LINT_CACHE="$CS_TREE/build/good.json" STUB_LINT_REPO_ROOT="$CS_TREE" \
    python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
    >"$TMP_DIR/cs-zres.out" 2>"$TMP_DIR/cs-zres.err"
CS_ZRES_RC=$?
if [ "$CS_ZRES_RC" = "8" ]; then
    t_pass "shared cache schema: a zero-RESOLVED baseline is rejected too"
else
    t_fail "shared cache schema: zero-resolved baseline gave rc=$CS_ZRES_RC (want 8); $(head -2 "$TMP_DIR/cs-zres.err")"
fi
cat > "$CS_TREE/scripts/lint/stub-lint-baseline.json" <<'JSON'
{"resolved": 1, "total": 1}
JSON

# 23l: MUTATION-CHECK THE RULE ITSELF. Delete the falsey-shape check from the
# shared module and 23b must go red. A fixture that passes with the rule removed
# is testing nothing -- this is the check that proves it is not.
#
# MUTATED AGAINST THE LINT, not the snapshot, and the difference is the point.
# With the rule deleted, `stamped_items: {}` reaches `enumerate({})`, which
# yields nothing -- so the SNAPSHOT still refuses at rc 3, but via its
# zero-population guard rather than the deleted rule. It would look like a
# passing mutation-check while proving nothing about the rule (observed while
# writing this fixture). The lint is the side where this rule is uniquely
# load-bearing: it has no population guard by design (see 23j), so deleting the
# rule restores exactly the original defect -- a silent clean run over zero
# refs. rc must therefore move OFF 5.
#
# A whole mini-TREE is copied, not one module: BOTH readers derive their import
# root from their own file location (`Path(__file__).resolve().parent` /
# `parents[2]`), which puts the REAL scripts/todo-graph ahead of PYTHONPATH, so
# a mutant reachable only via PYTHONPATH is never imported.
CS_MUT="$TMP_DIR/cs-mutant"
rm -rf "$CS_MUT"; mkdir -p "$CS_MUT/scripts/todo-graph" "$CS_MUT/scripts/lint"
cp "$REPO_ROOT"/scripts/todo-graph/*.py "$CS_MUT/scripts/todo-graph/"
cp "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" "$CS_MUT/scripts/lint/"
CS_MUT_MOD="$CS_MUT/scripts/todo-graph"
python3 - "$CS_MUT_MOD/cache_schema.py" <<'PY'
import re, sys
p = sys.argv[1]
src = open(p).read()
# Neuter ONLY the stamped_items list-type check (the 23b rule).
# Indent is 4, not 8: section 19 extracted this walk into
# `_validate_stamped_items`. It must appear EXACTLY once, so a future move
# cannot silently select a different rule.
needle = """    if not isinstance(items, list):"""
assert src.count(needle) == 1, (
    f"mutation target appears {src.count(needle)}x -- cache_schema.py moved")
src = src.replace(needle, """    if False:""", 1)
open(p, "w").write(src)
PY
# THE MUTATION STEP MUST SUCCEED. When the needle went stale (section 19's
# extraction re-indented it), the assert above fired, the heredoc exited 1, and
# the mutant file was left UNMUTATED -- yet the case still reported PASS,
# because the unmutated copy happened to exit non-5 for an unrelated reason.
# A mutation fixture that is green when no mutation was applied proves nothing;
# this is the same "passes on any failure" shape flagged against 22af/22ag.
if [ $? -ne 0 ]; then
    t_fail "shared cache schema: mutation-check could not apply its mutation (stale needle)"
fi
python3 - "$CS_TREE/build/good.json" "$CS_TREE/build/mut.json" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
doc[0]["stamped_items"] = {}
json.dump(doc, open(sys.argv[2], "w"))
PY
bind_cache "$CS_TREE/build/mut.json" "$CS_TREE/todo"
STUB_LINT_CACHE="$CS_TREE/build/mut.json" STUB_LINT_REPO_ROOT="$CS_TREE" \
    STUB_LINT_ALLOW_NO_BASELINE=1 \
    python3 "$CS_MUT/scripts/lint/check_stub_behind_stamp.py" \
    >"$TMP_DIR/cs-mut.out" 2>"$TMP_DIR/cs-mut.err"
CS_MUT_RC=$?
# Sanity: the UNMUTATED lint must reject the same fixture at 5, or "5 -> not 5"
# below would be measuring the fixture rather than the rule.
STUB_LINT_CACHE="$CS_TREE/build/mut.json" STUB_LINT_REPO_ROOT="$CS_TREE" \
    STUB_LINT_ALLOW_NO_BASELINE=1 \
    python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
    >/dev/null 2>&1
CS_REAL_RC=$?
if [ "$CS_REAL_RC" = "5" ] && [ "$CS_MUT_RC" != "5" ]; then
    t_pass "shared cache schema: mutation-check -- deleting the falsey rule reopens the silent-clean-run defect"
else
    t_fail "shared cache schema: mutation-check FAILED -- real lint rc=$CS_REAL_RC (want 5), mutant rc=$CS_MUT_RC (want anything but 5)"
fi

# 23l2: MUTATION-CHECK EVERY CLAIMED RULE, not just the falsey one. The
# checklist says the bad-shape fixtures are "each mutation-checked"; 23l alone
# covered only the stamped_items list guard, which overstated the evidence
# (Codex consistency, section 17 review). Each rule below is neutered in a COPY
# of the module and the matching fixture must stop being rejected. Driven
# through validate_nodes directly so a rule is tested in isolation rather than
# through whichever reader-level guard happens to fire first.
CS_MUT2=$(python3 - "$REPO_ROOT" 2>&1 <<'PY'
import copy, json, re, sys, types
repo = sys.argv[1]
src = open(repo + "/scripts/todo-graph/cache_schema.py").read()

GOOD = [{"file_path": "todo/01-test/TODO-01-cs.md",
         "stamped_items": [{"section_n": 1, "item_idx": 0,
                            "item_text": "shipped a thing",
                            "refs": [{"kind": "symbol", "file": "src/a.c",
                                      "symbol": "f"}]}]}]

# Section 19 subtree fixtures. Each carries ONLY the subtree its profile
# declares, which also proves the profiles are independent: neither of these
# documents would survive the default stamped-items profile.
# `status` is "x", NOT "[x]": the producer strips the brackets
# (`build.py` matches `\[([ x/])\]` and stores the inner character). This
# fixture originally carried "[x]" and the validator ACCEPTED it, which is
# precisely the hole the closed-domain check now closes -- the consumer tests
# `status not in ("x", "/")`, so "[x]" read as UNFINISHED.
GOOD_SECTIONS = [{"file_path": "todo/01-test/TODO-01-cs.md",
                  "sections": [{"n": 1, "deliverable": "a thing",
                                "depends_on": [], "status": "x"}]}]

GOOD_XREFS = [{"file_path": "todo/01-test/TODO-01-cs.md",
               "stamps_xrefs": [{"kind": "accepted", "severity": "high",
                                 "target_path": "todo/01-test/TODO-02-x.md",
                                 "target_section": "3"}]}]

# Section 22 fixture. ONE COMPLETE NODE, because PROFILE_QUERY declares FOUR
# subtrees (sections, sections.depends_on, stamps_xrefs, node_fields) and every
# one of them runs on every node -- a fixture satisfying only the subtree under
# test is refused by the other three, which makes the mutation unmeasurable.
# `GOOD_DEPS` and `GOOD_FIELDS` are the same node; the two names just say which
# rule the cases below are aiming at.
GOOD_QUERY = [{"file_path": "todo/01-test/TODO-01-cs.md",
               "id": "cs", "domain": "01-test", "status": "draft",
               "title": "cs", "created_at": "2026-04-01T00:00:00Z",
               "last_active_at": "2026-04-01T00:00:00Z",
               "sections": [{"n": 1, "deliverable": "a thing", "status": "x",
                             "depends_on": [{"target": "TODO-02-x",
                                             "sections": [3]}]}],
               "inputs_xrefs": [{"target_path": "todo/01-test/TODO-02-x.md",
                                 "target_section": "3"}],
               "stamps_xrefs": []}]
GOOD_DEPS = GOOD_QUERY
GOOD_FIELDS = GOOD_QUERY

def mutate(doc, path, value, delete=False):
    d = copy.deepcopy(doc)
    tgt = d
    for k in path[:-1]:
        tgt = tgt[k]
    if delete:
        del tgt[path[-1]]
    else:
        tgt[path[-1]] = value
    return d

# (label, rule source line to neuter, fixture, profile-attr-or-None)
#
# NEEDLES MUST BE UNIQUE IN THE MODULE. Section 19 extracted the stamped-items
# walk into `_validate_stamped_items` and added `_validate_sections` /
# `_validate_stamps_xrefs`, which (a) re-indented every target here and (b) made
# the bare `if extra:` line ambiguous across three validators -- `.replace(...,
# 1)` would have neutered whichever came FIRST in the file, silently testing the
# wrong rule while still reporting a pass. The additionalProperties cases
# therefore neuter their `extra = [...]` comprehension, which names its own key
# tuple and so cannot collide.
CASES = [
    ("stamped_items list guard", "    if not isinstance(items, list):",
     mutate(GOOD, [0, "stamped_items"], {}), None),
    ("refs list guard", "        if not isinstance(refs, list):",
     mutate(GOOD, [0, "stamped_items", 0, "refs"], {}), None),
    ("required item fields", "            if field not in it:",
     mutate(GOOD, [0, "stamped_items", 0, "item_text"], None, delete=True), None),
    ("item additionalProperties",
     "        extra = [k for k in it if k not in _ITEM_KEYS]",
     mutate(GOOD, [0, "stamped_items", 0, "zzz"], 1), None),
    ("file_path required-string", '        if "file_path" not in node:',
     mutate(GOOD, [0, "file_path"], None, delete=True), None),
    ("empty node array", "    if not nodes:", [], None),
    # Section 19 subtree validators. Each runs under the profile that DECLARES
    # its subtree -- under the default profile these fixtures are accepted,
    # which is itself the point of the profile split.
    ("sections list guard", "    if not isinstance(sections, list):",
     mutate(GOOD_SECTIONS, [0, "sections"], {}), "PROFILE_SECTIONS"),
    ("required section fields", "            if field not in s:",
     mutate(GOOD_SECTIONS, [0, "sections", 0, "status"], None, delete=True),
     "PROFILE_SECTIONS"),
    ("section additionalProperties",
     "        extra = [k for k in s if k not in _SECTION_KEYS]",
     mutate(GOOD_SECTIONS, [0, "sections", 0, "zzz"], 1), "PROFILE_SECTIONS"),
    ("section depends_on list guard",
     '        if not isinstance(s["depends_on"], list):',
     mutate(GOOD_SECTIONS, [0, "sections", 0, "depends_on"], "x"),
     "PROFILE_SECTIONS"),
    ("section status closed domain",
     "        if s[\"status\"] not in _SECTION_STATUSES:",
     mutate(GOOD_SECTIONS, [0, "sections", 0, "status"], "[x]"),
     "PROFILE_SECTIONS"),
    ("section n uniqueness", "            if s[\"n\"] in seen_n:",
     [{"file_path": "todo/01-test/TODO-01-cs.md",
       "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": "x"},
                    {"n": 1, "deliverable": "b", "depends_on": [], "status": " "}]}],
     "PROFILE_SECTIONS"),
    # Section 22 subtree validators, under PROFILE_QUERY. Each fixture is
    # ACCEPTED by PROFILE_SECTIONS (which does not open a dependency group) and
    # refused here, which is the profile split doing its job one more time.
    # The group OBJECT guard is deliberately absent from this list. Its fixture
    # (a non-dict group) also trips the required-fields rule that follows it --
    # `"target" not in "TODO-02-x"` is True for a string -- so neutering the
    # guard alone still refuses, and the harness cannot attribute the refusal.
    # That rule is covered end-to-end instead, by the nested-malformed cache
    # fixtures in sub-tests 9v/9w, which assert the refusal a real caller sees.
    ("required section-dep fields", "                if field not in grp:",
     mutate(GOOD_DEPS, [0, "sections", 0, "depends_on", 0, "target"], None,
            delete=True), "PROFILE_QUERY"),
    ("section-dep additionalProperties",
     "            extra = [x for x in grp if x not in _SECTION_DEP_KEYS]",
     mutate(GOOD_DEPS, [0, "sections", 0, "depends_on", 0, "zzz"], 1),
     "PROFILE_QUERY"),
    ("section-dep sections list guard",
     "            if not isinstance(secs, list):",
     mutate(GOOD_DEPS, [0, "sections", 0, "depends_on", 0, "sections"], 3),
     "PROFILE_QUERY"),
    ("node required-string fields", "        if field not in node:",
     mutate(GOOD_FIELDS, [0, "domain"], None, delete=True), "PROFILE_QUERY"),
    # The scalar value's CHARACTERS must be distinct: with the list guard
    # neutered the walk iterates the string, and a repeated character would be
    # caught by the uniqueItems rule instead -- making the fixture pass for the
    # wrong reason and reporting this rule as unverified.
    ("node optional list guard",
     "        if not isinstance(value, list):",
     mutate(GOOD_FIELDS, [0, "depends_on"], "abc"), "PROFILE_QUERY"),
    ("node uniqueItems members", "            if item in seen_members:",
     mutate(GOOD_FIELDS, [0, "depends_on"], ["a", "a"]), "PROFILE_QUERY"),
    ("inputs_xrefs list guard", "    if not isinstance(inputs, list):",
     mutate(GOOD_FIELDS, [0, "inputs_xrefs"], {}), "PROFILE_QUERY"),
    ("required inputs-xref fields", "            if key not in entry:",
     mutate(GOOD_FIELDS, [0, "inputs_xrefs", 0, "target_path"], None,
            delete=True), "PROFILE_QUERY"),
    ("inputs-xref additionalProperties",
     "        extra = [k for k in entry if k not in _INPUTS_XREF_KEYS]",
     mutate(GOOD_FIELDS, [0, "inputs_xrefs", 0, "zzz"], 1), "PROFILE_QUERY"),
    ("stamps_xrefs list guard", "    if not isinstance(xrefs, list):",
     mutate(GOOD_XREFS, [0, "stamps_xrefs"], {}), "PROFILE_STAMP_XREFS"),
    ("required stamp-xref fields", "            if field not in x:",
     mutate(GOOD_XREFS, [0, "stamps_xrefs", 0, "kind"], None, delete=True),
     "PROFILE_STAMP_XREFS"),
    ("stamp-xref additionalProperties",
     "        extra = [k for k in x if k not in _STAMP_XREF_KEYS]",
     mutate(GOOD_XREFS, [0, "stamps_xrefs", 0, "zzz"], 1), "PROFILE_STAMP_XREFS"),
]

def load(mutated_src):
    mod = types.ModuleType("cs_mut")
    mod.__dict__["__file__"] = repo + "/scripts/todo-graph/cache_schema.py"
    exec(compile(mutated_src, "cs_mut", "exec"), mod.__dict__)
    return mod

def neuter(needle):
    """The replacement that disables exactly this rule and nothing else.

    An `if ...:` guard becomes `if False:`. An additionalProperties rule is
    identified by its `extra = [...]` comprehension (the only unique text now
    that three validators carry an `if extra:`), and is disabled by emptying the
    comprehension rather than by deleting the branch that reads it.
    """
    indent = needle[:len(needle) - len(needle.lstrip())]
    body = "extra = []" if needle.lstrip().startswith("extra = ") else "if False:"
    return indent + body

real = load(src)
bad = []
for label, needle, fixture, profile_attr in CASES:
    if src.count(needle) != 1:
        bad.append(f"{label}: mutation target appears {src.count(needle)}x "
                   f"-- needle is not unique, or the module moved")
        continue
    kw = {}
    if profile_attr is not None:
        if not hasattr(real, profile_attr):
            bad.append(f"{label}: module exposes no {profile_attr}")
            continue
        kw["profile"] = getattr(real, profile_attr)
    # Baseline: the REAL module must reject this fixture, and WITH WHICH REASON
    # matters -- each reason maps to a different documented exit code, so a rule
    # that merely changes EMPTY into LEGACY has changed an ERROR into a WARN.
    try:
        real.validate_nodes(copy.deepcopy(fixture), "x", **kw)
        bad.append(f"{label}: real module ACCEPTED the fixture (fixture is inert)")
        continue
    except real.CacheSchemaError as exc:
        real_reason = exc.reason
    # Neuter just this rule; the fixture must then get through.
    mut = load(src.replace(needle, neuter(needle), 1))
    mkw = ({"profile": getattr(mut, profile_attr)}
           if profile_attr is not None else {})
    try:
        mut.validate_nodes(copy.deepcopy(fixture), "x", **mkw)
    except mut.CacheSchemaError as exc:
        # A rejection with the SAME reason means some OTHER rule was doing the
        # work and this fixture proves nothing about the named one. A DIFFERENT
        # reason is a pass: the rule decided which documented exit code the
        # caller reports. Deleting the empty-array rule, for instance, leaves
        # `[]` rejected as LEGACY (lint rc 3, a WARN) instead of EMPTY (rc 5,
        # an ERROR) -- the input is still refused, but at the wrong severity.
        if exc.reason == real_reason:
            bad.append(f"{label}: still rejected as {exc.reason} with the "
                       f"rule deleted")
    except Exception:
        # Any OTHER exception means the rule IS load-bearing: without it the
        # bad shape reaches the walk and crashes it, which is precisely the
        # uncaught-traceback-as-rc-1 failure the validator exists to prevent.
        # (Deleting the required-field check makes `it["item_text"]` a
        # KeyError, for example.) That is a passing mutation, not a failure.
        pass
print("OK" if not bad else "FAIL: " + "; ".join(bad))
PY
)
if [ "$CS_MUT2" = "OK" ]; then
    t_pass "shared cache schema: every claimed rule is individually mutation-checked"
else
    t_fail "shared cache schema: per-rule mutation check -- $CS_MUT2"
fi

# 23m: THE SHARED MODULE IS IN THE IDENTITY-GATE CLOSURE. It is imported by the
# differentially-executed snapshot and can decide whether that snapshot RUNS AT
# ALL, so a cache-schema-only change must not satisfy the gate's byte-identical
# early exit and execute neither reader (Codex design review, section 17).
if grep -q '"scripts/todo-graph/cache_schema.py"' \
        "$REPO_ROOT/scripts/todo-graph/identity-gate.sh"; then
    t_pass "shared cache schema: module is in the identity-gate CLOSURE"
else
    t_fail "shared cache schema: cache_schema.py missing from identity-gate.sh CLOSURE -- a schema-only change would skip the walk"
fi

# 23n: BOTH READERS ACTUALLY IMPORT THE SHARED MODULE. Without this, every
# assertion above could be satisfied by two private copies that happen to agree
# today -- which is the exact state section 17 exists to end.
if grep -q "^import cache_schema as _cs" \
        "$REPO_ROOT/scripts/todo-graph/corpus_resolution_snapshot.py" \
   && grep -q "import cache_schema as _cs" \
        "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py"; then
    t_pass "shared cache schema: both readers import the one validator"
else
    t_fail "shared cache schema: a reader is not importing cache_schema -- the rule is not actually shared"
fi

# ----------------------------------------------------------------------
# THE NEWLY ROUTED READERS. One fixture per reader asserting that reader's OWN
# infrastructure code -- not merely "non-zero". A reader whose cache failure
# lands on a VERDICT code has not been routed, it has been disguised, and only
# an exact-code assertion catches that.
#
# The DOWNSTREAM caller is asserted too. Routing a reader fail-closed is
# worthless while its caller fails open, which is exactly what lint Check 24
# did: it discarded stderr, erased the status with `|| true`, and substituted
# `{}` for empty output, so every infrastructure refusal reported zero findings
# on the every-commit path (Codex adversarial, section 19, [high]).
# ----------------------------------------------------------------------
RR_TREE="$TMP_DIR/routed-readers"
mkdir -p "$RR_TREE/todo/01-test" "$RR_TREE/build"
cat > "$RR_TREE/todo/01-test/TODO-01-rr.md" <<'MD'
# TODO-01 routed-reader fixture

## Implementation Order

| 💎 | 1 | a thing | -- | [x] |

## 1. A section

- [x] done
MD

rr_case() {
    local label="$1" content="$2" want="$3"
    printf '%s' "$content" > "$RR_TREE/build/todo-cache.json"
    bind_cache "$RR_TREE/build/todo-cache.json" "$RR_TREE/todo"
    ( cd "$RR_TREE" && python3 "$REPO_ROOT/scripts/todo-reachability.py" \
        "$RR_TREE/todo/01-test/TODO-01-rr.md" ) >/dev/null 2>&1
    local rc=$?
    if [ "$rc" = "$want" ]; then
        t_pass "routed reader: todo-reachability $label -> rc $rc"
    else
        t_fail "routed reader: todo-reachability $label -> rc $rc (want $want)"
    fi
}

# EXIT_INFRA (2) is DISTINCT from the FINDINGS code (1) this tool already used.
rr_case "unparseable cache" 'not json' 2
rr_case "empty node array" '[]' 2
rr_case "node missing file_path" '[{"sections": []}]' 2
rr_case "sections wrong-typed" \
    '[{"file_path": "todo/01-test/TODO-01-rr.md", "sections": {}}]' 2
# The two semantic constraints, both of which a shape-only check would accept.
rr_case "duplicate section number" \
    '[{"file_path": "todo/01-test/TODO-01-rr.md", "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": "x"}, {"n": 1, "deliverable": "b", "depends_on": [], "status": " "}]}]' 2
rr_case "status outside the producer domain" \
    '[{"file_path": "todo/01-test/TODO-01-rr.md", "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": "[x]"}]}]' 2
# POSITIVE CONTROL. Without it every rc-2 above could be firing for an unrelated
# reason, and the COVERAGE fixtures below would prove nothing.
rr_case "a valid cache covering the file's IO row is ACCEPTED" \
    '[{"file_path": "todo/01-test/TODO-01-rr.md", "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": "x"}]}]' 0
# NODE IDENTITY. A second node naming the same file overwrites the first in
# every consumer's dict, so a decoy node could replace a file's real statuses.
rr_case "duplicate node file_path" \
    '[{"file_path": "todo/01-test/TODO-01-rr.md", "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": "x"}]}, {"file_path": "todo/01-test/TODO-01-rr.md", "sections": [{"n": 1, "deliverable": "b", "depends_on": [], "status": " "}]}]' 2
# CPU BUDGET. Section numbers are hashed as set members and dict keys; unbounded
# values can be chosen to collide (cache_schema._MAX_SECTION_N).
rr_case "section number past the ceiling" \
    '[{"file_path": "todo/01-test/TODO-01-rr.md", "sections": [{"n": 2305843009213693951, "deliverable": "a", "depends_on": [], "status": "x"}]}]' 2
# COVERAGE. Both shapes are SCHEMA-VALID and both used to resolve to a status of
# "", which reads as "not done" and silently suppressed the section's findings.
rr_case "node omits a status for a row the file declares" \
    '[{"file_path": "todo/01-test/TODO-01-rr.md", "sections": []}]' 2
rr_case "cache has no node at all for the audited file" \
    '[{"file_path": "todo/01-test/TODO-99-other.md", "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": "x"}]}]' 2
# NEGATIVE for the coverage rule: a null `n` is LEGAL (an unparseable Section
# column) and must not be mistaken for a missing row -- the row it would answer
# for is one `_io_rows` cannot produce either.
# SENTINEL COLLISION. `file_path` is any non-empty string, so a node named
# after the tool's own "no cache" marker used to switch the entire audit onto
# its weak pre-cache fallback with a cache loaded, past every check above.
rr_case "a node named __missing__ cannot fake an absent cache" \
    '[{"file_path": "__missing__", "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": "x"}]}]' 2
# ALIAS DECOY. A second SPELLING of a file already in the cache is not a
# duplicate `file_path`, so node identity cannot see it; refusing non-canonical
# identities at load is what closes it.
rr_case "an absolute-path node identity is refused" \
    "[{\"file_path\": \"$RR_TREE/todo/01-test/TODO-01-rr.md\", \"sections\": [{\"n\": 1, \"deliverable\": \"a\", \"depends_on\": [], \"status\": \" \"}]}, {\"file_path\": \"todo/01-test/TODO-01-rr.md\", \"sections\": [{\"n\": 1, \"deliverable\": \"a\", \"depends_on\": [], \"status\": \"x\"}]}]" 2
rr_case "a dot-dot node identity is refused" \
    '[{"file_path": "todo/01-test/../01-test/TODO-01-rr.md", "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": " "}]}]' 2
# THE COLLAPSING SPELLINGS. Each is a DISTINCT file_path string that normalizes
# onto the canonical key, so no duplicate rule sees a repeat -- the decoy is
# placed second in every pair, which is the order that overwrites.
rr_case "a leading ./ node identity is refused" \
    '[{"file_path": "todo/01-test/TODO-01-rr.md", "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": "x"}]}, {"file_path": "./todo/01-test/TODO-01-rr.md", "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": " "}]}]' 2
rr_case "a doubled-separator node identity is refused" \
    '[{"file_path": "todo/01-test/TODO-01-rr.md", "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": "x"}]}, {"file_path": "todo//01-test/TODO-01-rr.md", "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": " "}]}]' 2
rr_case "an embedded ./ node identity is refused" \
    '[{"file_path": "todo/01-test/./TODO-01-rr.md", "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": " "}]}]' 2
rr_case "a trailing-separator node identity is refused" \
    '[{"file_path": "todo/01-test/TODO-01-rr.md/", "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": " "}]}]' 2
rr_case "a null section number is dropped, not refused" \
    '[{"file_path": "todo/01-test/TODO-01-rr.md", "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": "x"}, {"n": null, "deliverable": "unparseable row", "depends_on": [], "status": " "}]}]' 0

# NEGATIVE: a MISSING cache is not corruption. The per-file fallback must keep
# working on a fresh clone, so this stops the routing from being over-eager.
rm -f "$RR_TREE/build/todo-cache.json"
( cd "$RR_TREE" && python3 "$REPO_ROOT/scripts/todo-reachability.py" \
    "$RR_TREE/todo/01-test/TODO-01-rr.md" ) >/dev/null 2>&1
RR_MISS_RC=$?
if [ "$RR_MISS_RC" != "2" ]; then
    t_pass "routed reader: todo-reachability falls back on a MISSING cache (rc $RR_MISS_RC)"
else
    t_fail "routed reader: todo-reachability treated an ABSENT cache as corruption"
fi

# MUTATION: restore the raw load the routing replaced. The refusal must vanish.
RR_MUT="$TMP_DIR/rr-mutant"
rm -rf "$RR_MUT"; mkdir -p "$RR_MUT/build"
cp -r "$RR_TREE/todo" "$RR_MUT/todo"
RR_MUT_OK=$(python3 - "$REPO_ROOT/scripts/todo-reachability.py" \
    "$RR_MUT/todo-reachability.py" <<'PY'
import sys
src = open(sys.argv[1]).read()
needle = "        data, info = _cs.load_and_validate("
if src.count(needle) != 1:
    print(f"FAIL: routing call site appears {src.count(needle)}x -- moved")
    raise SystemExit(0)
raw = ('        import json as _rawjson\n'
       '        data = _rawjson.loads(cache_path.read_text(encoding="utf-8"))\n'
       '        info = type("I", (), {"corpus": None})()\n'
       '        _unused = (lambda *a, **k: None)(')
open(sys.argv[2], "w").write(src.replace(needle, raw, 1))
print("OK")
PY
)
if [ "$RR_MUT_OK" != "OK" ]; then
    t_fail "routed reader: mutation could not be applied -- $RR_MUT_OK"
else
    printf 'not json' > "$RR_MUT/build/todo-cache.json"
    ( cd "$RR_MUT" && python3 "$RR_MUT/todo-reachability.py" \
        "$RR_MUT/todo/01-test/TODO-01-rr.md" ) >/dev/null 2>&1
    RR_MUT_RC=$?
    if [ "$RR_MUT_RC" != "2" ]; then
        t_pass "routed reader: mutation-check -- the raw load reopens the silent degrade (rc $RR_MUT_RC)"
    else
        t_fail "routed reader: mutation-check FAILED -- raw load still refused at 2, so the fixture is not testing the routing"
    fi
fi

# The delegation prober maps a cache refusal to its OWN infrastructure code (3),
# never to its VIOLATION code (1) -- which would report a corrupt cache as
# "the consumer bypasses the shared rule".
printf 'not json' > "$RR_TREE/build/todo-cache.json"
STUB_LINT_CACHE="$RR_TREE/build/todo-cache.json" STUB_LINT_REPO_ROOT="$RR_TREE" \
    python3 "$REPO_ROOT/scripts/lint/check_consumer_delegation.py" >/dev/null 2>&1
RR_DELEG_RC=$?
if [ "$RR_DELEG_RC" = "3" ]; then
    t_pass "routed reader: check_consumer_delegation maps a corrupt cache to rc 3, not its violation code"
else
    t_fail "routed reader: check_consumer_delegation corrupt cache -> rc $RR_DELEG_RC (want 3)"
fi

# DOWNSTREAM: lint Check 24 must SURFACE the refusal, not absorb it.
#
# STRUCTURAL, and deliberately so: driving the real Check 24 needs a full
# `lint.sh` run over 786 files against a deliberately-corrupted cache, which is
# far too heavy for this suite and would race any concurrent run. What it pins
# is the EXACT regression -- the reachability invocation must not discard the
# status. The behavior was verified directly when the fix landed: with a
# corrupt cache, `lint.sh` reports
# `error: scripts/todo-reachability.py:0: reachability audit could not run
# (rc 2): ... [UNREADABLE] ...` and exits 1, while a clean tree still exits 0
# with 0 errors.
# Scoped to the whole Check 24 BLOCK, not one physical line: the first cut
# grepped only the line carrying `--json`, so moving `|| true` onto the
# following continuation line evaded it entirely, and the error string
# appearing ANYWHERE in lint.sh satisfied it (Codex adversarial round 2,
# [medium]).
LINT24_BLOCK=$(python3 - "$REPO_ROOT/scripts/lint.sh" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read().splitlines()
start = next((i for i, l in enumerate(src) if "todo-reachability.py --json" in l), None)
if start is None:
    print("MISSING"); raise SystemExit(0)
# Walk back to the enclosing `if`, forward to its `fi`, so continuation lines
# and the whole handling block are inside the window.
lo = max(0, start - 12)
# WIDENED 2026-08-08 (45 -> 90). The window is a stand-in for "the enclosing
# block", and the block grew when Check 24 learned to downgrade a STALE refusal
# at the commit gate: the `reachability audit could not run` emission moved past
# +45 and this pin reported `captures the code but never reports it` about code
# that reports it two lines later. A window is not a parser -- if this fires
# again, check whether the string simply moved before believing the finding.
hi = min(len(src), start + 90)
block = "\n".join(src[lo:hi])
problems = []
if re.search(r"todo-reachability\.py --json[^\n]*\n?[^\n]*\|\|\s*true", block):
    problems.append("discards the exit code with '|| true'")
if "LINT24_RC" not in block:
    problems.append("never captures the exit code")
if "reachability audit could not run" not in block:
    problems.append("captures the code but never reports it")
if "LINT24_SHAPE" not in block:
    problems.append("accepts rc 1 without validating the JSON envelope "
                    "(an uncaught exception also exits 1)")
print("OK" if not problems else "; ".join(problems))
PY
)
case "$LINT24_BLOCK" in
    OK) t_pass "routed reader: lint Check 24 surfaces a reachability infrastructure refusal" ;;
    MISSING) t_fail "routed reader: lint Check 24 no longer invokes todo-reachability -- the every-commit gate is gone" ;;
    *) t_fail "routed reader: lint Check 24 fails open -- $LINT24_BLOCK" ;;
esac

# ----------------------------------------------------------------------
# TRANSACTIONAL LOAD. The rr_case fixtures above each run a FRESH process, so
# none of them can see the in-process state this asserts: a refusal part-way
# through the node walk must not leave a usable prefix behind for the next call
# in the same interpreter to serve (Codex re-adversarial round 3, [medium]).
# ----------------------------------------------------------------------
RR_RETRY=$(python3 - "$REPO_ROOT" "$RR_TREE" <<'PY'
import importlib.util, json, os, sys
from pathlib import Path
root, tree = Path(sys.argv[1]), Path(sys.argv[2])
spec = importlib.util.spec_from_file_location(
    "tr", root / "scripts" / "todo-reachability.py")
tr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tr)
# A VALID node first, then one with a non-canonical identity: the valid prefix
# is exactly what a non-transactional load would publish before refusing.
(tree / "build" / "todo-cache.json").write_text(json.dumps([
    {"file_path": "todo/01-test/TODO-01-rr.md",
     "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": "x"}]},
    {"file_path": "./todo/01-test/TODO-02-rr.md",
     "sections": [{"n": 1, "deliverable": "b", "depends_on": [], "status": " "}]},
]))
problems = []
for attempt in (1, 2):
    try:
        tr._load_cache(str(tree))
        problems.append(f"attempt {attempt}: the refusal did not raise")
    except tr.CacheUnusable:
        pass
    except Exception as exc:                                  # noqa: BLE001
        problems.append(f"attempt {attempt}: wrong exception {exc!r}")
if tr._CACHE:
    problems.append(f"a refused load published {len(tr._CACHE)} node(s)")

# MISSING -> LOADED, in one process. A cache created by a concurrent build
# between two calls must not publish nodes and a fingerprint while the audit
# keeps answering from the weak no-cache fallback; the verdict is pinned at the
# first settled outcome instead.
spec2 = importlib.util.spec_from_file_location(
    "tr2", root / "scripts" / "todo-reachability.py")
tr2 = importlib.util.module_from_spec(spec2)
cache_file = tree / "build" / "todo-cache.json"
saved = cache_file.read_text()
cache_file.unlink()
spec2.loader.exec_module(tr2)
tr2._load_cache(str(tree))
if not tr2._CACHE_LOADED["missing"]:
    problems.append("an absent cache did not record the missing verdict")
cache_file.write_text(json.dumps([
    {"file_path": "todo/01-test/TODO-01-rr.md",
     "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": "x"}]},
]))
tr2._load_cache(str(tree))
if tr2._CACHE or tr2._CACHE_CORPUS:
    problems.append("a pinned missing verdict still published cache state")
if tr2._io_status("todo/01-test/TODO-01-rr.md", str(tree), (1,)) is not None:
    problems.append("a pinned missing verdict still answered from the cache")
cache_file.write_text(saved)

# ROOT BINDING. A resolved verdict belongs to the tree it was taken from; the
# same process auditing a SECOND tree must not be served the first one's nodes,
# and a pinned "missing" under one root must not make a CORRUPT cache under
# another read as absent.
spec3 = importlib.util.spec_from_file_location(
    "tr3", root / "scripts" / "todo-reachability.py")
tr3 = importlib.util.module_from_spec(spec3)
spec3.loader.exec_module(tr3)
tree_b = tree.parent / "routed-readers-b"
(tree_b / "build").mkdir(parents=True, exist_ok=True)
(tree_b / "todo" / "01-test").mkdir(parents=True, exist_ok=True)
(tree_b / "todo" / "01-test" / "TODO-01-rr.md").write_text(
    (tree / "todo" / "01-test" / "TODO-01-rr.md").read_text())
cache_file.write_text(json.dumps([
    {"file_path": "todo/01-test/TODO-01-rr.md",
     "sections": [{"n": 1, "deliverable": "a", "depends_on": [], "status": "x"}]},
]))
tr3._load_cache(str(tree))
if tr3._CACHE_LOADED["root"] != str(tree.resolve()):
    problems.append("a resolved verdict is not bound to its root")
(tree_b / "build" / "todo-cache.json").write_text("not json")
try:
    tr3._load_cache(str(tree_b))
    problems.append("a corrupt cache under a second root was served from the first")
except tr3.CacheUnusable:
    pass
# ...and the missing-then-corrupt order, which is the one that suppressed the
# refusal outright.
spec4 = importlib.util.spec_from_file_location(
    "tr4", root / "scripts" / "todo-reachability.py")
tr4 = importlib.util.module_from_spec(spec4)
spec4.loader.exec_module(tr4)
(tree_b / "build" / "todo-cache.json").unlink()
tr4._load_cache(str(tree_b))
if not tr4._CACHE_LOADED["missing"]:
    problems.append("root B's absent cache did not record the missing verdict")
# Root A's cache is CORRUPT. A missing verdict pinned under B must not answer
# for A -- that is exactly how the required infrastructure refusal disappears.
cache_file.write_text("not json")
try:
    tr4._load_cache(str(tree))
    problems.append("a pinned missing verdict swallowed another root's corrupt cache")
except tr4.CacheUnusable:
    pass
except Exception as exc:                                      # noqa: BLE001
    problems.append(f"unexpected {exc!r}")
cache_file.write_text(saved)

# CWD-VS-ROOT. The body is read relative to the process cwd and the status came
# from `root`; a raw-spelling fallback let those be two different trees.
spec5 = importlib.util.spec_from_file_location(
    "tr5", root / "scripts" / "todo-reachability.py")
tr5 = importlib.util.module_from_spec(spec5)
spec5.loader.exec_module(tr5)
(tree_b / "build" / "todo-cache.json").write_text(json.dumps([
    {"file_path": "todo/01-test/TODO-01-rr.md",
     "sections": [{"n": 1, "deliverable": "b", "depends_on": [], "status": " "}]},
]))
cwd = os.getcwd()
os.chdir(tree)
try:
    tr5._load_cache(str(tree_b))
    tr5._io_status("todo/01-test/TODO-01-rr.md", str(tree_b), (1,))
    problems.append("a body under root A was answered from root B's cache")
except tr5.CacheUnusable:
    pass
except Exception as exc:                                      # noqa: BLE001
    problems.append(f"unexpected {exc!r}")
finally:
    os.chdir(cwd)
print("OK" if not problems else "; ".join(problems))
PY
)
case "$RR_RETRY" in
    OK) t_pass "routed reader: a refused cache load publishes nothing and re-refuses on retry" ;;
    *) t_fail "routed reader: cache load is not transactional -- $RR_RETRY" ;;
esac

# ----------------------------------------------------------------------
# SCHEMA <-> PYTHON PARITY FOR THE SECTION-19 SUBTREES, asserted MECHANICALLY
# rather than by inspection. `stamped_items` had this from section 17 via
# cs_case; `sections` and `stamps_xrefs` shipped without it, so a rule living in
# only one of the two places was undetectable -- and one did: the Python walk
# required `n >= 0` while the published schema permitted any integer, so `n: -1`
# satisfied the contract and made the consumer refuse (Codex consistency,
# section 19 review, [medium]).
#
# Each row mutates ONE field of a known-good subtree and asserts BOTH oracles
# reach the same verdict. The documented exception is listed as data, not
# hidden: `n` uniqueness is a per-node semantic invariant JSON Schema cannot
# express (no uniqueness-by-property), so it is Python-only BY DESIGN and the
# fixture asserts exactly that asymmetry rather than ignoring it.
# ----------------------------------------------------------------------
CS_PARITY=$(python3 - "$REPO_ROOT" <<'PY'
import json, sys
from pathlib import Path
root = Path(sys.argv[1])
sys.path.insert(0, str(root / "scripts" / "todo-graph"))
import cache_schema as cs
try:
    import jsonschema
except ImportError:
    print("SKIP jsonschema not installed")
    raise SystemExit(0)

schema = json.loads((root / cs.SCHEMA_REL).read_text())
GOOD_SECTION = {"n": 1, "deliverable": "a", "depends_on": [], "status": "x"}
GOOD_XREF = {"kind": "accepted", "severity": "M",
             "target_path": "todo/01-test/TODO-01.md", "target_section": "2",
             "item_name": "a thing"}

def mk(subtree, value):
    node = {"file_path": "todo/01-test/TODO-01.md", subtree: value}
    return [node]

# (label, subtree, value, profile, python_only)
CASES = [
    ("sections good", "sections", [dict(GOOD_SECTION)], cs.PROFILE_SECTIONS, False),
    ("sections n null", "sections", [dict(GOOD_SECTION, n=None)], cs.PROFILE_SECTIONS, False),
    ("sections n negative", "sections", [dict(GOOD_SECTION, n=-1)], cs.PROFILE_SECTIONS, False),
    ("sections n over ceiling", "sections", [dict(GOOD_SECTION, n=cs._MAX_SECTION_N + 1)], cs.PROFILE_SECTIONS, False),
    ("sections n bool", "sections", [dict(GOOD_SECTION, n=True)], cs.PROFILE_SECTIONS, False),
    ("sections bad status", "sections", [dict(GOOD_SECTION, status="[x]")], cs.PROFILE_SECTIONS, False),
    ("sections extra key", "sections", [dict(GOOD_SECTION, wat=1)], cs.PROFILE_SECTIONS, False),
    ("sections missing deliverable", "sections", [{"n": 1, "depends_on": [], "status": "x"}], cs.PROFILE_SECTIONS, False),
    ("sections depends_on wrong type", "sections", [dict(GOOD_SECTION, depends_on={})], cs.PROFILE_SECTIONS, False),
    ("sections duplicate n", "sections", [dict(GOOD_SECTION), dict(GOOD_SECTION, deliverable="b")], cs.PROFILE_SECTIONS, True),
    ("xrefs good", "stamps_xrefs", [dict(GOOD_XREF)], cs.PROFILE_STAMP_XREFS, False),
    ("xrefs item_name omitted", "stamps_xrefs", [{k: v for k, v in GOOD_XREF.items() if k != "item_name"}], cs.PROFILE_STAMP_XREFS, False),
    ("xrefs missing target_path", "stamps_xrefs", [{k: v for k, v in GOOD_XREF.items() if k != "target_path"}], cs.PROFILE_STAMP_XREFS, False),
    ("xrefs extra key", "stamps_xrefs", [dict(GOOD_XREF, wat=1)], cs.PROFILE_STAMP_XREFS, False),
    ("xrefs non-object entry", "stamps_xrefs", ["nope"], cs.PROFILE_STAMP_XREFS, False),
    ("xrefs kind wrong type", "stamps_xrefs", [dict(GOOD_XREF, kind=3)], cs.PROFILE_STAMP_XREFS, False),
]

bad = []
for label, subtree, value, profile, python_only in CASES:
    try:
        cs.validate_nodes(mk(subtree, value), "fixture", profile)
        py = "accept"
    except cs.CacheSchemaError:
        py = "reject"
    try:
        jsonschema.validate(value, schema["items"]["properties"][subtree])
        js = "accept"
    except jsonschema.ValidationError:
        js = "reject"
    if python_only:
        if not (py == "reject" and js == "accept"):
            bad.append(f"{label}: expected a Python-only rule, got py={py} schema={js}")
    elif py != js:
        bad.append(f"{label}: python={py} schema={js}")
print("OK" if not bad else "; ".join(bad))
PY
)
# The label states the split rather than claiming blanket agreement. 15 cases
# must produce IDENTICAL verdicts; `sections duplicate n` is declared
# Python-only because JSON Schema 2020-12 cannot express uniqueness-by-property
# (`uniqueItems` is whole-item equality and the two rows differ in
# `deliverable`), and that case is asserted in BOTH directions above -- it fails
# if the schema starts rejecting it OR if the Python walk stops. Saying "agree"
# for all 16 was the overclaim (Codex consistency, section 19 review, [medium]).
case "$CS_PARITY" in
    OK) t_pass "shared cache schema: sections + stamps_xrefs -- 15 cases verdict-identical to the published schema, 1 declared Python-only (duplicate n)" ;;
    SKIP*) t_fail "shared cache schema: parity table could not run -- $CS_PARITY" ;;
    *) t_fail "shared cache schema: schema/Python drift -- $CS_PARITY" ;;
esac

# ----------------------------------------------------------------------
# ROUTED READERS NAME SUBTREES BY THE SHARED CONSTANT, NOT A RETYPED LITERAL.
# The vocabulary is centralized at `cache_schema.SUBTREE_*`, but a reader that
# retypes the string reintroduces exactly what centralizing it removed: a
# rename would validate one field while the reader accessed another, so
# `todo-reachability` escapes through an uncaught KeyError under its VERDICT
# code and `check_consumer_delegation` silently counts zero (Codex consistency,
# section 19 review, [medium]).
#
# Asserted over the AST, NOT by text search. Both files legitimately mention
# these names in comments and error messages -- a grep for the literal would
# fail on prose and force the check to be muted, which is how a real rule turns
# into an ignored one. Only the ACCESS shapes are examined: `node["sections"]`
# and `node.get("stamped_items", ...)`.
# ----------------------------------------------------------------------
RR_CONST=$(python3 - "$REPO_ROOT" <<'PY'
import ast, sys
from pathlib import Path
root = Path(sys.argv[1])
sys.path.insert(0, str(root / "scripts" / "todo-graph"))
import cache_schema as cs

NAMES = {cs.SUBTREE_STAMPED_ITEMS, cs.SUBTREE_SECTIONS, cs.SUBTREE_STAMPS_XREFS}
TARGETS = ["scripts/todo-reachability.py", "scripts/lint/check_consumer_delegation.py"]

bad = []
for rel in TARGETS:
    path = root / rel
    if not path.is_file():
        bad.append(f"{rel}: missing")
        continue
    tree = ast.parse(path.read_text(encoding="utf-8"), str(path))
    for node in ast.walk(tree):
        # node["sections"]
        if isinstance(node, ast.Subscript) and isinstance(node.slice, ast.Constant) \
                and node.slice.value in NAMES:
            bad.append(f"{rel}:{node.lineno}: subscripts a bare "
                       f"{node.slice.value!r}; use cache_schema.SUBTREE_*")
        # node.get("stamped_items", ...)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) \
                and node.func.attr == "get" and node.args \
                and isinstance(node.args[0], ast.Constant) \
                and node.args[0].value in NAMES:
            bad.append(f"{rel}:{node.lineno}: .get() on a bare "
                       f"{node.args[0].value!r}; use cache_schema.SUBTREE_*")
print("OK" if not bad else "; ".join(bad))
PY
)
case "$RR_CONST" in
    OK) t_pass "routed reader: subtrees named by the shared SUBTREE_* constants, no retyped literals" ;;
    *) t_fail "routed reader: retyped subtree literal -- $RR_CONST" ;;
esac

# ----------------------------------------------------------------------
# 25: THE PRODUCER GENERATION WINDOW (section 21).
#
# Section 17 made both READERS generation-bound and could not close the mirror
# image: `build.py` reads a TODO, that TODO changes, and `build.py` then
# publishes a cache whose newer mtime made the already-stale node look FRESH to
# every freshness test -- legitimately, because the cache really was newer. The
# refusal has to live in the producer, and these fixtures are what prove it does.
#
# THE MUTATION IS THE POINT. Each case is re-run against a COPY of build.py with
# the re-verification deleted, and the fixture must FAIL there. A refusal fixture
# that also passes against a producer with no refusal in it is measuring the
# exit code of something else.
# ----------------------------------------------------------------------
PW_TREE="$TMP_DIR/producer-window"
mkdir -p "$PW_TREE/todo/01-test" "$PW_TREE/build" "$PW_TREE/mut/scripts/todo-graph"
cat > "$PW_TREE/todo/01-test/TODO-01-pw.md" <<'MD'
---
id: todo-01-pw
schema_version: 1
domain: 01-test
status: active
title: producer window fixture
---
# TODO-01 producer-window fixture

## 1. Section
- [x] shipped a thing
MD
cat > "$PW_TREE/todo/01-test/TODO-02-pw.md" <<'MD'
---
id: todo-02-pw
schema_version: 1
domain: 01-test
status: active
title: producer window fixture two
---
# TODO-02 producer-window fixture

## 1. Section
- [x] shipped another thing
MD

# The mutant producer: the same file with the corpus re-verification removed.
# Copied, never edited in place -- a harness that restores with `git checkout`
# destroys uncommitted work, which this repo has paid for once already.
cp "$REPO_ROOT/scripts/todo-graph/build.py" "$PW_TREE/mut/scripts/todo-graph/build.py"
cp "$REPO_ROOT/scripts/todo-graph/cache_schema.py" "$PW_TREE/mut/scripts/todo-graph/cache_schema.py"
# The SCHEMA travels with them. build.py cross-checks _ALWAYS_EMITTED against
# the `required` list in schema/cache.schema.json, resolved beside the
# cache_schema module it imports -- so a mut tree carrying the two .py files and
# not the contract they are checked against is an INCOMPLETE install, and every
# producer-window mutant died on FileNotFoundError instead of running.
mkdir -p "$PW_TREE/mut/scripts/todo-graph/schema"
cp "$REPO_ROOT/scripts/todo-graph/schema/cache.schema.json" \
   "$PW_TREE/mut/scripts/todo-graph/schema/cache.schema.json"
PW_MUT_OK=$(python3 - "$PW_TREE/mut/scripts/todo-graph/build.py" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
src = p.read_text()
needle = "        _cs.check_corpus_unchanged(todo_root, consumed)\n"
if needle not in src:
    print("STALE-NEEDLE")
else:
    p.write_text(src.replace(needle, "        pass\n", 1))
    print("OK")
PY
)
if [ "$PW_MUT_OK" != "OK" ]; then
    t_fail "producer window: mutation could not be applied (stale needle: $PW_MUT_OK)"
fi

# pw_run <producer-build.py> <mutation-python> -> prints "<rc>|<cache-sha-or-NONE>"
#
# The mutation runs from inside `build_node`, i.e. after that file's bytes have
# been read and before the cache is published -- exactly the window under test.
# It is handed the CALL INDEX (`n`, 1-based) plus both TODO paths and their
# original text, because WHICH call fires is the whole distinction between the
# cases: mutating a file the pass has already consumed is 25a, while mutating
# one it has not reached yet and restoring it afterwards is the ABA sequence.
# `walk_todo_files` sorts, so call 1 is TODO-01 and call 2 is TODO-02.
pw_run() {
    PW_BUILD="$1" PW_TREE="$PW_TREE" PW_MUT="$2" python3 - <<'PY'
import hashlib, importlib.util, os, pathlib, sys
build_py = os.environ["PW_BUILD"]
tree = pathlib.Path(os.environ["PW_TREE"])
sys.path.insert(0, str(pathlib.Path(build_py).parent))
spec = importlib.util.spec_from_file_location("pw_build", build_py)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
t1 = tree / "todo/01-test/TODO-01-pw.md"
t2 = tree / "todo/01-test/TODO-02-pw.md"
o1, o2 = t1.read_text(), t2.read_text()
real = mod.build_node
state = {"n": 0}
def hooked(file_path, *a, **k):
    state["n"] += 1
    exec(os.environ["PW_MUT"],
         {"n": state["n"], "t1": t1, "t2": t2, "o1": o1, "o2": o2})
    return real(file_path, *a, **k)
mod.build_node = hooked
out = tree / "build/todo-cache.json"
sys.argv = ["build.py", "--quiet", "--root", str(tree / "todo"),
            "--repo-root", str(tree), "--output", str(out)]
try:
    rc = mod.main()
finally:
    t1.write_text(o1)
    t2.write_text(o2)
sha = (hashlib.sha256(out.read_bytes()).hexdigest()[:16] if out.exists() else "NONE")
print(f"{rc}|{sha}")
PY
}

# Seed a good cache so "the previous cache survives byte-identical" is testable.
python3 "$REPO_ROOT/scripts/todo-graph/build.py" --quiet \
    --root "$PW_TREE/todo" --repo-root "$PW_TREE" \
    --output "$PW_TREE/build/todo-cache.json" >/dev/null 2>&1
PW_BEFORE=$(sha256sum "$PW_TREE/build/todo-cache.json" | cut -c1-16)
PW_BINDINGS_BEFORE=$(ls "$PW_TREE/build/" | grep -c 'corpus-' || true)

# 25a: A TODO CHANGED MID-BUILD IS REFUSED AT rc 3, AND NOTHING IS WRITTEN.
PW_A=$(pw_run "$REPO_ROOT/scripts/todo-graph/build.py" \
    "t1.write_text(o1 + '\n- [ ] appeared mid-build\n') if n == 2 else None")
PW_AFTER=$(sha256sum "$PW_TREE/build/todo-cache.json" | cut -c1-16)
# THE BINDING IS PART OF "UNTOUCHED". A refusal that left the cache intact but
# removed or replaced its binding would leave a cache no reader can read -- the
# window widened, not closed. PW_BINDINGS_BEFORE was recorded and never asserted
# until Codex pointed it out (section 21, [medium]).
PW_BINDINGS_AFTER=$(ls "$PW_TREE/build/" | grep -c 'corpus-' || true)
PW_BOUND_OK=$([ -f "$PW_TREE/build/todo-cache.json.corpus-$PW_BEFORE.json" ] && echo yes || echo no)
if [ "$PW_A" = "3|$PW_BEFORE" ] && [ "$PW_AFTER" = "$PW_BEFORE" ] \
        && [ "$PW_BOUND_OK" = "yes" ] && [ "$PW_BINDINGS_AFTER" = "$PW_BINDINGS_BEFORE" ]; then
    t_pass "producer window: a TODO changed mid-build refuses at rc 3, prior cache AND binding untouched"
else
    t_fail "producer window: mid-build change gave '$PW_A' (want '3|$PW_BEFORE'), cache now $PW_AFTER (was $PW_BEFORE), binding present=$PW_BOUND_OK, bindings $PW_BINDINGS_AFTER (was $PW_BINDINGS_BEFORE)"
fi

# 25b: THE ABA CASE -- changed to B, read as B, restored to A before the check.
# Boundary scans compared to each other pass this: both see A. Only a
# fingerprint taken from the bytes actually CONSUMED sees that B was parsed.
PW_B=$(pw_run "$REPO_ROOT/scripts/todo-graph/build.py" \
    "t2.write_text(o2 + '\n- [ ] B\n') if n == 1 else t2.write_text(o2)")
if [ "${PW_B%%|*}" = "3" ]; then
    t_pass "producer window: the ABA sequence (A -> B -> A) is refused, not certified"
else
    t_fail "producer window: ABA sequence gave '$PW_B' (want rc 3)"
fi

# 25c: THE MUTATION. The same two cases against a producer whose re-verification
# was deleted must SUCCEED -- if they still refuse, these fixtures are measuring
# something other than the check they claim to.
PW_MA=$(pw_run "$PW_TREE/mut/scripts/todo-graph/build.py" \
    "t1.write_text(o1 + '\n- [ ] appeared mid-build\n') if n == 2 else None")
if [ "${PW_MA%%|*}" = "0" ]; then
    t_pass "producer window: mutation check -- deleting the re-verify makes 25a pass a stale cache"
else
    t_fail "producer window: MUTATION SURVIVED -- mutant producer gave '$PW_MA' (want rc 0)"
fi
# 25c2: AND A MUTANT THAT ISOLATES THE ABA PROPERTY. Deleting the re-verify
# outright (25c) shows the check exists; it does not show WHERE the fingerprint
# comes from. This mutant keeps the re-verify and only moves the fingerprint
# back to a scan taken BEFORE the extraction pass -- the boundary-scan design.
# 25a must still refuse there (the file stays changed, so the two scans differ),
# while the ABA case must slip through (both scans see A). If 25b passes against
# this mutant too, it is not testing what it claims.
cp "$REPO_ROOT/scripts/todo-graph/build.py" "$PW_TREE/mut/scripts/todo-graph/build-aba.py"
PW_ABA_OK=$(python3 - "$PW_TREE/mut/scripts/todo-graph/build-aba.py" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
src = p.read_text()
hashline = "        consumed[rel_key] = hashlib.sha256(raw_bytes).hexdigest()\n"
initline = "    consumed = {}\n"
prescan = "    consumed = _cs._scan_corpus(todo_root, lambda e: None)\n"
if hashline not in src or initline not in src:
    print("STALE-NEEDLE")
else:
    src = src.replace(hashline, "        pass\n", 1)
    src = src.replace(initline, prescan, 1)
    p.write_text(src)
    print("OK")
PY
)
if [ "$PW_ABA_OK" != "OK" ]; then
    t_fail "producer window: ABA mutation could not be applied (stale needle: $PW_ABA_OK)"
fi
PW_MB=$(pw_run "$PW_TREE/mut/scripts/todo-graph/build-aba.py" \
    "t2.write_text(o2 + '\n- [ ] B\n') if n == 1 else t2.write_text(o2)")
PW_MB2=$(pw_run "$PW_TREE/mut/scripts/todo-graph/build-aba.py" \
    "t1.write_text(o1 + '\n- [ ] appeared mid-build\n') if n == 2 else None")
if [ "${PW_MB%%|*}" = "0" ] && [ "${PW_MB2%%|*}" = "3" ]; then
    t_pass "producer window: mutation check -- a boundary-scan fingerprint lets the ABA sequence through"
else
    t_fail "producer window: ABA MUTATION SURVIVED -- boundary-scan mutant gave ABA='$PW_MB' (want rc 0), mid-build='$PW_MB2' (want rc 3)"
fi

# Restore the good cache the mutant just overwrote.
python3 "$REPO_ROOT/scripts/todo-graph/build.py" --quiet \
    --root "$PW_TREE/todo" --repo-root "$PW_TREE" \
    --output "$PW_TREE/build/todo-cache.json" >/dev/null 2>&1

# 25d: THE PUBLISHED PAIR IS SELF-DESCRIBING, and old bindings do not pile up.
# Digest-named bindings are what let the sidecar be written BEFORE the cache
# without a two-file transaction, so exactly one must survive each publish.
PW_SHA=$(sha256sum "$PW_TREE/build/todo-cache.json" | cut -c1-16)
PW_N=$(ls "$PW_TREE/build/" | grep -c 'corpus-' || true)
if [ -f "$PW_TREE/build/todo-cache.json.corpus-$PW_SHA.json" ] && [ "$PW_N" = "1" ]; then
    t_pass "producer window: the binding is named for the cache it describes, one per publish"
else
    t_fail "producer window: binding for $PW_SHA missing or $PW_N bindings present (want 1)"
fi

# 25e: AND THE PUBLISHED PAIR IS ACCEPTED BY THE READERS. Every case above is a
# refusal; without this one they would all still pass against a producer that
# refused unconditionally.
PW_READ=$(python3 -c "
import pathlib, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
p = pathlib.Path('$PW_TREE/build/todo-cache.json')
try:
    cs.check_freshness(p, '$PW_TREE/todo', cache_bytes=p.read_bytes())
    print('FRESH')
except cs.CacheSchemaError as e:
    print(e.reason, e)
" 2>&1)
if [ "$PW_READ" = "FRESH" ]; then
    t_pass "producer window: a cache this producer published is accepted by the shared reader"
else
    t_fail "producer window: freshly built cache gave '$PW_READ' (want FRESH)"
fi

# 25f: HISTORY IS BOUND TOO. `created_at`/`last_active_at` come from the
# corpus-limited git log, so identical BYTES over a moved history still means
# stale nodes -- a content-only binding would certify it (Codex design review,
# section 21, [high]).
#
# THE HISTORY IS MOVED FOR REAL, not by editing the recorded field. Rewriting
# the sidecar to a sentinel proves only that two strings compare unequal, which
# is true of any field and would pass against a binding that recorded something
# irrelevant (Codex adversarial, section 21, [medium]).
PW_GIT="$TMP_DIR/producer-window-git"
mkdir -p "$PW_GIT/build"
cp -r "$PW_TREE/todo" "$PW_GIT/todo"
(
    cd "$PW_GIT" || exit 1
    git init -q . && git config user.email "t@example.invalid" \
        && git config user.name "producer window fixture" \
        && git add -A && git commit -q --no-verify -m "corpus"
) >/dev/null 2>&1
python3 "$REPO_ROOT/scripts/todo-graph/build.py" --quiet \
    --root "$PW_GIT/todo" --repo-root "$PW_GIT" \
    --output "$PW_GIT/build/todo-cache.json" >/dev/null 2>&1
PW_GIT_FRESH=$(python3 -c "
import pathlib, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
p = pathlib.Path('$PW_GIT/build/todo-cache.json')
try:
    cs.check_freshness(p, '$PW_GIT/todo', cache_bytes=p.read_bytes())
    print('FRESH')
except cs.CacheSchemaError as e:
    print(e.reason)
" 2>&1)
# A commit that touches the corpus WITHOUT changing any TODO byte: the content
# fingerprint is identical afterwards, so only the history binding can see it.
(
    cd "$PW_GIT" || exit 1
    # Edit and commit, then revert and commit: two commits touching the corpus,
    # and a working tree byte-identical to the one the cache was built from. The
    # content fingerprint cannot see this; last_active_at moved regardless.
    cp todo/01-test/TODO-02-pw.md "$TMP_DIR/pw-02-original"
    printf '\n- [ ] briefly\n' >> todo/01-test/TODO-02-pw.md
    git commit -q --no-verify -am "corpus edit" \
        && git revert --no-edit HEAD
) >/dev/null 2>&1
# THE REVERT MUST HAVE RESTORED THE BYTES, or this fixture silently degrades
# into a CONTENT test and would pass against a build with no history binding at
# all. (`git revert -q` is not a valid flag -- it exits 129 -- which is exactly
# how this fixture first "passed" for the wrong reason.)
if ! cmp -s "$PW_GIT/todo/01-test/TODO-02-pw.md" "$TMP_DIR/pw-02-original"; then
    t_fail "producer window: history fixture did not restore the corpus bytes -- it would be testing content, not history"
fi
PW_HIST=$(python3 -c "
import pathlib, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
p = pathlib.Path('$PW_GIT/build/todo-cache.json')
try:
    cs.check_freshness(p, '$PW_GIT/todo', cache_bytes=p.read_bytes())
    print('NORAISE')
except cs.CacheSchemaError as e:
    print(e.reason)
" 2>&1)
if [ "$PW_GIT_FRESH" = "FRESH" ] && [ "$PW_HIST" = "STALE" ]; then
    t_pass "producer window: a moved corpus git history refuses even on identical bytes"
else
    t_fail "producer window: history binding gave before='$PW_GIT_FRESH' (want FRESH), after='$PW_HIST' (want STALE)"
fi

# 25g: AN EMPTY CORPUS IS REFUSED BY THE PRODUCER, not just by readers.
# check_freshness always refused to certify a cache against an empty corpus, so
# a producer that published `[]` emitted an artifact every reader rejects -- the
# two halves of one rule disagreeing (Codex adversarial, section 21, [high]).
mkdir -p "$TMP_DIR/pw-empty/todo" "$TMP_DIR/pw-empty/build"
python3 "$REPO_ROOT/scripts/todo-graph/build.py" --quiet \
    --root "$TMP_DIR/pw-empty/todo" --repo-root "$TMP_DIR/pw-empty" \
    --output "$TMP_DIR/pw-empty/build/todo-cache.json" >/dev/null 2>&1
PW_EMPTY_RC=$?
if [ "$PW_EMPTY_RC" = "3" ] && [ ! -f "$TMP_DIR/pw-empty/build/todo-cache.json" ]; then
    t_pass "producer window: an empty corpus is refused at rc 3 with nothing written"
else
    t_fail "producer window: empty corpus gave rc=$PW_EMPTY_RC (want 3), cache present=$([ -f "$TMP_DIR/pw-empty/build/todo-cache.json" ] && echo yes || echo no)"
fi

# 25h: A NON-REGULAR CORPUS ENTRY IS REFUSED, not read. os.walk reports a
# symlink to a FIFO as an ordinary file, so hashing it would block every reader
# forever -- a denial of service needing no overflow or overread (Codex
# adversarial, section 21, [medium]).
PW_FIFO="$TMP_DIR/pw-fifo"
mkdir -p "$PW_FIFO/todo/01-test"
cp "$PW_TREE/todo/01-test/TODO-01-pw.md" "$PW_FIFO/todo/01-test/TODO-01-pw.md"
mkfifo "$PW_FIFO/pipe" 2>/dev/null && ln -s "$PW_FIFO/pipe" "$PW_FIFO/todo/01-test/TODO-99-fifo.md"
if [ -p "$PW_FIFO/pipe" ]; then
    PW_FIFO_OUT=$(timeout 20 python3 -c "
import sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
try:
    cs._scan_corpus('$PW_FIFO/todo', lambda e: None)
    print('NORAISE')
except cs.CacheSchemaError as e:
    print(e.reason)
" 2>&1)
    PW_FIFO_RC=$?
    if [ "$PW_FIFO_OUT" = "STALE" ]; then
        t_pass "producer window: a corpus entry that is not a regular file is refused, not read"
    elif [ "$PW_FIFO_RC" = "124" ]; then
        t_fail "producer window: hashing a FIFO corpus entry HUNG (timed out) -- the type check is missing"
    else
        t_fail "producer window: FIFO corpus entry gave '$PW_FIFO_OUT' (want STALE)"
    fi
else
    t_pass "producer window: FIFO corpus case skipped (mkfifo unavailable on this filesystem)"
fi

# 25i: THE WRAPPER PROPAGATES build.py STATUS. build-and-validate.sh flattened
# every nonzero result to 1 while its own header promised propagation, which
# makes a RETRYABLE rc 3 (the corpus moved) indistinguishable from a rc 1 that
# will fail identically forever (Codex adversarial, section 21, [medium]).
# Driven with a stub producer so the assertion is about the WRAPPER, and so it
# cannot be satisfied by whatever the real corpus happens to do today.
BV_TREE="$TMP_DIR/bv-propagate"
mkdir -p "$BV_TREE/scripts/todo-graph" "$BV_TREE/build"
cp "$REPO_ROOT/scripts/todo-graph/build-and-validate.sh" "$BV_TREE/scripts/todo-graph/"
cp "$REPO_ROOT/scripts/todo-graph/validate.py" "$BV_TREE/scripts/todo-graph/"
bv_rc_for() {   # bv_rc_for <exit-code-the-stub-producer-returns>
    printf '#!/usr/bin/env python3\nimport sys\nsys.exit(%s)\n' "$1" \
        > "$BV_TREE/scripts/todo-graph/build.py"
    bash "$BV_TREE/scripts/todo-graph/build-and-validate.sh" >/dev/null 2>&1
    echo $?
}
BV_3=$(bv_rc_for 3)
BV_1=$(bv_rc_for 1)
if [ "$BV_3" = "3" ] && [ "$BV_1" = "1" ]; then
    t_pass "producer window: build-and-validate.sh propagates build.py exit status (3 stays 3)"
else
    t_fail "producer window: wrapper flattened build.py status -- rc3 -> '$BV_3' (want 3), rc1 -> '$BV_1' (want 1)"
fi

# 25j: AN UNBORN HEAD IS A LEGITIMATE TREE. `git init` plus TODO files but no
# commit leaves `rev-parse --git-dir` succeeding while `log`/`rev-list HEAD`
# both fail. Reporting that as an indeterminate git failure refused a tree that
# `collect_git_timestamps` already handles by emitting None timestamps -- the
# producer would exit 3 and every freshness-enabled reader would hard-fail on a
# brand-new repository (Codex adversarial, section 21, [medium]).
PW_UNBORN="$TMP_DIR/pw-unborn"
mkdir -p "$PW_UNBORN/build"
cp -r "$PW_TREE/todo" "$PW_UNBORN/todo"
(cd "$PW_UNBORN" && git init -q .) >/dev/null 2>&1
python3 "$REPO_ROOT/scripts/todo-graph/build.py" --quiet \
    --root "$PW_UNBORN/todo" --repo-root "$PW_UNBORN" \
    --output "$PW_UNBORN/build/todo-cache.json" >/dev/null 2>&1
PW_UNBORN_RC=$?
PW_UNBORN_ID=$(python3 -c "
import sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
try:
    print(cs.corpus_history_id('$PW_UNBORN/todo'))
except cs.CacheSchemaError as e:
    print('RAISED')
" 2>&1)
# ...and gaining the first commit MOVES it, so the cache built before that
# commit is correctly seen as stale by a history-consuming reader.
(
    cd "$PW_UNBORN" || exit 1
    git config user.email "t@example.invalid" && git config user.name "unborn fixture"
    git add -A && git commit -q --no-verify -m "first commit"
) >/dev/null 2>&1
PW_UNBORN_AFTER=$(python3 -c "
import sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
try:
    print(cs.corpus_history_id('$PW_UNBORN/todo'))
except cs.CacheSchemaError:
    print('RAISED')
" 2>&1)
# AN INDETERMINATE GIT FAILURE RAISES rather than becoming another sentinel.
# Collapsing "could not ask" into a stable value would let a producer and a
# reader compare EQUAL during the same failure and certify nodes whose timestamp
# extraction failed (Codex re-adversarial, section 21, [medium]). Injected: a
# corrupt HEAD is NOT usable for this, because git reports it as "not a git
# repository", which is a determinate answer this code correctly returns.
PW_INDET=$(python3 -c "
import subprocess, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
real = subprocess.run
def hooked(cmd, *a, **k):
    r = real(cmd, *a, **k)
    if isinstance(cmd, list) and 'rev-list' in cmd:
        return subprocess.CalledProcessError(128, cmd, output='', stderr='fatal: injected')
    return r
subprocess.run = hooked
try:
    print('RETURNED', cs.corpus_history_id('$PW_GIT/todo'))
except cs.CacheSchemaError as e:
    print('RAISED')
except Exception as e:
    print('ESCAPED', type(e).__name__)
finally:
    subprocess.run = real
" 2>&1)
if [ "$PW_UNBORN_RC" = "0" ] && [ "$PW_UNBORN_ID" = "unborn-head" ] \
        && [ "$PW_UNBORN_AFTER" != "unborn-head" ] && [ "$PW_INDET" = "RAISED" ]; then
    t_pass "producer window: unborn HEAD is determinate, the first commit moves it, an indeterminate failure raises"
else
    t_fail "producer window: unborn=rc$PW_UNBORN_RC/'$PW_UNBORN_ID' (want 0/unborn-head), after-first-commit='$PW_UNBORN_AFTER' (want != unborn-head), indeterminate='$PW_INDET' (want RAISED)"
fi

# 25k: THE RECORDED HISTORY IS THE ONE THE TIMESTAMPS CAME FROM. Pinning it
# with a SEPARATE `git log` observes a DIFFERENT generation: move history to B
# while the timestamp walk resolves, take B-derived timestamps, move back to A,
# and an A-vs-A comparison publishes B's timestamps under an A binding (Codex
# re-adversarial, section 21, [high]).
#
# STAGED DETERMINISTICALLY rather than by racing a real commit: the timestamp
# walk's own output is doctored to describe a generation one commit ahead, while
# the tree stays where it is. That is exactly the state an A-to-B-to-A sequence
# leaves behind, and it cannot flake.
PW_ABA_HIST=$(PW_GIT="$PW_GIT" REPO_ROOT="$REPO_ROOT" PW_BUILD="$REPO_ROOT/scripts/todo-graph/build.py" python3 - <<'PY' 2>&1
import importlib.util, os, pathlib, subprocess, sys
tree = pathlib.Path(os.environ["PW_GIT"])
repo = os.environ["REPO_ROOT"]
sys.path.insert(0, repo + "/scripts/todo-graph")
spec = importlib.util.spec_from_file_location("pw_aba_build", os.environ["PW_BUILD"])
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
real_run = mod.subprocess.run
def hooked(cmd, *a, **k):
    r = real_run(cmd, *a, **k)
    if isinstance(cmd, list) and "--name-only" in cmd:
        # The walk resolved against a generation with one MORE corpus commit.
        r.stdout = r.stdout + "COMMIT " + ("b" * 40) + " 1700000000\n"
    return r
mod.subprocess.run = hooked
out = tree / "build/aba-cache.json"
sys.argv = ["build.py", "--quiet", "--root", str(tree / "todo"),
            "--repo-root", str(tree), "--output", str(out)]
print("RESULT", mod.main(), out.exists())
PY
)
PW_ABA_HIST=$(printf '%s' "$PW_ABA_HIST" | grep '^RESULT ' | tail -1 | cut -d' ' -f2-)
if [ "$PW_ABA_HIST" = "3 False" ]; then
    t_pass "producer window: timestamps from a different history generation are refused, nothing written"
else
    t_fail "producer window: history-ABA gave '$PW_ABA_HIST' (want '3 False')"
fi

# 25k2: THE MUTANT. A producer that pins the history with its own
# corpus_history_id call instead of using the walk's provenance must PUBLISH the
# same staged state -- otherwise 25k is not measuring the provenance binding.
cp "$REPO_ROOT/scripts/todo-graph/build.py" "$PW_TREE/mut/scripts/todo-graph/build-hist.py"
PW_HIST_MUT=$(python3 - "$PW_TREE/mut/scripts/todo-graph/build-hist.py" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
src = p.read_text()
# The call sits inside a `try:` since section 31 (the walk can now REFUSE a
# shallow corpus), so the needle carries that indentation. The mutation itself
# is unchanged: pin the history with a separate `corpus_history_id` call
# instead of the walk's own provenance.
needle = ("        timestamps, history_id = collect_git_timestamps(\n"
          "            repo_root, files, todo_root)\n")
if needle not in src:
    print("STALE-NEEDLE")
else:
    p.write_text(src.replace(
        needle,
        "        timestamps, _ignored = collect_git_timestamps(\n"
        "            repo_root, files, todo_root)\n"
        "        history_id = _cs.corpus_history_id(todo_root)\n", 1))
    print("OK")
PY
)
if [ "$PW_HIST_MUT" != "OK" ]; then
    t_fail "producer window: history-ABA mutation could not be applied (stale needle: $PW_HIST_MUT)"
fi
PW_ABA_MUT=$(PW_GIT="$PW_GIT" REPO_ROOT="$REPO_ROOT" PW_BUILD="$PW_TREE/mut/scripts/todo-graph/build-hist.py" python3 - <<'PY' 2>&1
import importlib.util, os, pathlib, sys
tree = pathlib.Path(os.environ["PW_GIT"])
repo = os.environ["REPO_ROOT"]
sys.path.insert(0, repo + "/scripts/todo-graph")
spec = importlib.util.spec_from_file_location("pw_aba_mut", os.environ["PW_BUILD"])
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
real_run = mod.subprocess.run
def hooked(cmd, *a, **k):
    r = real_run(cmd, *a, **k)
    if isinstance(cmd, list) and "--name-only" in cmd:
        r.stdout = r.stdout + "COMMIT " + ("b" * 40) + " 1700000000\n"
    return r
mod.subprocess.run = hooked
out = tree / "build/aba-mut-cache.json"
sys.argv = ["build.py", "--quiet", "--root", str(tree / "todo"),
            "--repo-root", str(tree), "--output", str(out)]
print(mod.main())
PY
)
if [ "$PW_ABA_MUT" = "0" ]; then
    t_pass "producer window: mutation check -- a separately-pinned history publishes the wrong generation"
else
    t_fail "producer window: HISTORY MUTATION SURVIVED -- separately-pinned producer gave '$PW_ABA_MUT' (want 0)"
fi

# 25l: A PROFILE THAT DOES NOT CONSUME THE HISTORY-DERIVED FIELDS DOES NOT PAY
# FOR THEM. Both routed readers consume neither created_at nor last_active_at,
# and establishing history costs three git forks per call (Codex perf, section
# 21, [medium]). The gate must be the PROFILE, not a global switch: a profile
# that declares requires_history still refuses a moved history.
PW_PROF=$(python3 -c "
import pathlib, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
p = pathlib.Path('$PW_GIT/build/todo-cache.json')
b = p.read_bytes()
noh = cs.Profile('t-nohist', (cs.SUBTREE_SECTIONS,))
yeh = cs.Profile('t-hist', (cs.SUBTREE_SECTIONS,), requires_history=True)
def run(prof):
    try:
        cs.check_freshness(p, '$PW_GIT/todo', cache_bytes=b, profile=prof)
        return 'FRESH'
    except cs.CacheSchemaError as e:
        return e.reason
print(run(noh), run(yeh))
" 2>&1)
if [ "$PW_PROF" = "FRESH STALE" ]; then
    t_pass "producer window: history validation follows the caller profile, and still fires when declared"
else
    t_fail "producer window: profile-gated history gave '$PW_PROF' (want 'FRESH STALE')"
fi

# 25m: THE BINDING IS BOUNDED LIKE EVERY OTHER INPUT. It is read on every reader
# invocation, so an unbounded read of a corrupt or hostile sidecar is the one
# failure mode no reason code can report (Codex adversarial, section 21,
# [medium]). Asserted by shrinking the ceiling rather than writing a huge file.
PW_BOUND=$(python3 -c "
import pathlib, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
cs._MAX_SIDECAR_BYTES = 4
p = pathlib.Path('$PW_TREE/build/todo-cache.json')
try:
    cs.check_freshness(p, '$PW_TREE/todo', cache_bytes=p.read_bytes())
    print('NORAISE')
except cs.CacheSchemaError as e:
    print(e.reason, 'CEILING' if 'ceiling' in str(e) else 'OTHER')
" 2>&1)
# THE MESSAGE, not just the code: any refusal exits the same way, so asserting
# the reason alone would be satisfied by a fixture that never reached the bound.
if [ "$PW_BOUND" = "STALE CEILING" ]; then
    t_pass "producer window: an oversized corpus binding is a documented refusal, not an unbounded read"
else
    t_fail "producer window: oversized binding gave '$PW_BOUND' (want 'STALE CEILING')"
fi

# 25n: PRODUCER AND READER AGREE ON WHAT THE CORPUS IS -- for every entry shape,
# not just the easy one. Two divergences were live: `rglob` matched DIRECTORIES
# named TODO-x.md that the reader's `os.walk` reports under `dirs` (Codex
# consistency, [medium]); and skipping non-regular FILES made the producer fail
# OPEN where the reader fails closed, so a build SUCCEEDED and published a cache
# every reader then rejected (Codex re-adversarial, [medium]).
PW_DIR="$TMP_DIR/pw-dirname"
mkdir -p "$PW_DIR/todo/01-test/TODO-77-notafile.md" "$PW_DIR/build"
cp "$PW_TREE/todo/01-test/TODO-01-pw.md" "$PW_DIR/todo/01-test/TODO-01-pw.md"
ln -s "TODO-01-pw.md" "$PW_DIR/todo/01-test/TODO-02-link.md" 2>/dev/null
PW_DIR_OUT=$(PW_DIR="$PW_DIR" REPO_ROOT="$REPO_ROOT" python3 - <<'PY' 2>&1
import importlib.util, os, pathlib, sys
tree = pathlib.Path(os.environ["PW_DIR"])
repo = os.environ["REPO_ROOT"]
sys.path.insert(0, repo + "/scripts/todo-graph")
import cache_schema as cs
spec = importlib.util.spec_from_file_location(
    "pw_dir_build", repo + "/scripts/todo-graph/build.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
prod = {p.relative_to(tree / "todo").as_posix()
        for p in mod.walk_todo_files(tree / "todo")}
read = set(cs._scan_corpus(tree / "todo", lambda e: None))
print("SAME" if prod == read else f"DIVERGED prod={sorted(prod)} read={sorted(read)}")
PY
)
if [ "$PW_DIR_OUT" = "SAME" ]; then
    t_pass "producer window: producer and reader enumerate the same corpus (directory skipped, symlink-to-regular kept)"
else
    t_fail "producer window: enumerators diverge -- $PW_DIR_OUT"
fi

# 25n2: AND A NON-REGULAR ENTRY FAILS THE BUILD rather than being omitted from
# it. The reader refuses a FIFO named TODO-*.md, so a producer that silently
# skipped it published a cache nothing could read.
PW_FIFO2="$TMP_DIR/pw-fifo-producer"
mkdir -p "$PW_FIFO2/todo/01-test" "$PW_FIFO2/build"
cp "$PW_TREE/todo/01-test/TODO-01-pw.md" "$PW_FIFO2/todo/01-test/TODO-01-pw.md"
if mkfifo "$PW_FIFO2/todo/01-test/TODO-98-fifo.md" 2>/dev/null; then
    timeout 25 python3 "$REPO_ROOT/scripts/todo-graph/build.py" --quiet \
        --root "$PW_FIFO2/todo" --repo-root "$PW_FIFO2" \
        --output "$PW_FIFO2/build/todo-cache.json" >/dev/null 2>&1
    PW_FIFO2_RC=$?
    if [ "$PW_FIFO2_RC" = "124" ]; then
        t_fail "producer window: the producer HUNG on a FIFO corpus entry (timed out)"
    elif [ "$PW_FIFO2_RC" != "0" ] && [ ! -f "$PW_FIFO2/build/todo-cache.json" ]; then
        t_pass "producer window: a non-regular corpus entry FAILS the build instead of being skipped"
    else
        t_fail "producer window: FIFO corpus entry gave build rc=$PW_FIFO2_RC with cache present=$([ -f "$PW_FIFO2/build/todo-cache.json" ] && echo yes || echo no) (want nonzero + no cache)"
    fi
else
    t_pass "producer window: producer-side FIFO case skipped (mkfifo unavailable on this filesystem)"
fi

# ======================================================================
# Section 24: the history-identity cost decision
# ----------------------------------------------------------------------

# 24a: THE HISTORY AXIS IS THE PROFILE'S, AND IT DISCRIMINATES. A history moved
# under a byte-identical corpus must be REFUSED for a profile that declares
# requires_history and ACCEPTED for one that does not -- the same cache, the same
# instant, two answers. A fixture asserting only the refusal would pass just as
# well if the check had become unconditional, which is the regression that would
# put 47.9ms on all thirteen query verbs.
S24_TREE="$TMP_DIR/s24-history"
mkdir -p "$S24_TREE/build"
cp -r "$PW_TREE/todo" "$S24_TREE/todo"
(
    cd "$S24_TREE" || exit 1
    git init -q . && git config user.email "t@example.invalid" \
        && git config user.name "s24 fixture" \
        && git add -A && git commit -q --no-verify -m "corpus"
) >/dev/null 2>&1
python3 "$REPO_ROOT/scripts/todo-graph/build.py" --quiet \
    --root "$S24_TREE/todo" --repo-root "$S24_TREE" \
    --output "$S24_TREE/build/todo-cache.json" >/dev/null 2>&1
# CONTROL: the cache must exist before any verdict below means anything. A
# fixture whose build silently failed reports every profile as "refused" and
# looks like a pass of the strictest kind.
if [ -f "$S24_TREE/build/todo-cache.json" ]; then
    t_pass "section 24: control -- the history fixture built a cache to test against"
else
    t_fail "section 24: control -- the history fixture built NO cache, so every verdict below is vacuous"
fi
S24_PRE=$(python3 -c "
import pathlib, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
p = pathlib.Path('$S24_TREE/build/todo-cache.json')
_, i = cs.load_and_validate(p, '$S24_TREE/todo', profile=cs.PROFILE_QUERY_HISTORY)
_, j = cs.load_and_validate(p, '$S24_TREE/todo', profile=cs.PROFILE_QUERY)
# The history-consuming profile must CARRY the id forward for its post-walk
# re-check; the blind one must carry None so its re-check is a no-op, not a
# second 47.9ms probe.
print('HELD' if i.history else 'LOST', 'NONE' if j.history is None else 'PAID')
" 2>&1)
if [ "$S24_PRE" = "HELD NONE" ]; then
    t_pass "section 24: the validated history id rides on CacheInfo for the declaring profile only"
else
    t_fail "section 24: CacheInfo.history gave '$S24_PRE' (want 'HELD NONE')"
fi
# MUTATE: amend the commit. Every corpus byte is identical afterwards; only the
# history moved.
(
    cd "$S24_TREE" || exit 1
    git commit -q --no-verify --amend -m "corpus-amended"
) >/dev/null 2>&1
# Exercised through the CALLER PAIRING, not through load alone. Since the load
# defers the history comparison to a single post-walk probe (Codex perf,
# section 24, [medium]), `load_and_validate` + `check_history_unchanged` is the
# unit that carries the guarantee -- which is exactly the sequence query.py
# runs. Asserting on the load alone would now pin the wrong contract.
S24_POST=$(python3 -c "
import pathlib, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
p = pathlib.Path('$S24_TREE/build/todo-cache.json')
def run(prof):
    try:
        _, i = cs.load_and_validate(p, '$S24_TREE/todo', profile=prof)
        cs.check_history_unchanged('$S24_TREE/todo', i.history)
        cs.check_corpus_unchanged('$S24_TREE/todo', i.corpus)
        return 'ACCEPT'
    except cs.CacheSchemaError as e:
        return e.reason
print(run(cs.PROFILE_QUERY_HISTORY), run(cs.PROFILE_QUERY))
" 2>&1)
if [ "$S24_POST" = "STALE ACCEPT" ]; then
    t_pass "section 24: mutation check -- an amended history is refused for the history profile and invisible to the blind one"
else
    t_fail "section 24: moved history under identical bytes gave '$S24_POST' (want 'STALE ACCEPT')"
fi

# 24b: THE POST-WALK RE-CHECK COVERS THE HISTORY AXIS, NOT JUST THE BYTES. The
# window that matters is the caller's walk, and it was closed for content only:
# a rebase landing mid-walk leaves every byte identical while the timestamps the
# command already consumed stop describing the repository.
S24_WALK=$(python3 -c "
import pathlib, subprocess, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
root = '$S24_TREE/todo'
held = cs.corpus_history_id(pathlib.Path(root))
# Unchanged history: a no-op, and None is always a no-op.
try:
    cs.check_history_unchanged(root, held); a = 'OK'
except cs.CacheSchemaError as e:
    a = e.reason
try:
    cs.check_history_unchanged(root, None); b = 'NOOP'
except cs.CacheSchemaError as e:
    b = e.reason
# Now move it underneath, exactly as a mid-walk rebase would.
subprocess.run(['git', '-C', '$S24_TREE', 'commit', '-q', '--no-verify',
                '--amend', '-m', 'moved-mid-walk'],
               capture_output=True)
try:
    cs.check_history_unchanged(root, held); c = 'MISSED'
except cs.CacheSchemaError as e:
    c = e.reason
print(a, b, c)
" 2>&1)
if [ "$S24_WALK" = "OK NOOP STALE" ]; then
    t_pass "section 24: the post-walk history re-check is a no-op when unchanged or unrequested, and fires when the history moves mid-walk"
else
    t_fail "section 24: post-walk history re-check gave '$S24_WALK' (want 'OK NOOP STALE')"
fi

# 24c: AN INDETERMINATE TIMESTAMP WALK MUST NOT BECOME A VALID BINDING. A `git
# log` failure that clears before the caller's second probe used to publish null
# created_at/last_active_at under a perfectly valid history id, which a
# history-consuming reader then accepts as fresh (Codex design review, section
# 24, [high]). The two markers are distinct facts and only the determinate one
# may be rescued by a probe.
S24_PROV=$(python3 -c "
import sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import build as b
# The markers must be distinguishable from any real identity string. A real id
# is 'tip:count'; a bare string constant would be indistinguishable from one.
ok_type = (isinstance(b.PROVENANCE_INDETERMINATE, b._Provenance)
           and isinstance(b.PROVENANCE_NO_DERIVATION, b._Provenance)
           and not isinstance('deadbeef:12', b._Provenance))
# no-history:0 must NOT be a determinate rescue: the walk emits it itself on a
# successful empty read, so seeing it only from the fallback means the walk
# failed while git was healthy -- the transient case that must abort.
print('TYPED' if ok_type else 'UNTYPED',
      'no-repo' in b._DETERMINATE_NO_GIT and 'unborn-head' in b._DETERMINATE_NO_GIT
      and 'no-history:0' not in b._DETERMINATE_NO_GIT)
" 2>&1)
if [ "$S24_PROV" = "TYPED True" ]; then
    t_pass "section 24: provenance markers are typed and only no-repo/unborn-head may rescue an indeterminate walk"
else
    t_fail "section 24: provenance marker contract gave '$S24_PROV' (want 'TYPED True')"
fi
# MUTATION: a build whose timestamp walk fails while git is healthy must REFUSE.
# Forced by pointing the walk at a git that always fails, with the real git still
# answering the fallback probe.
S24_FAKEBIN="$TMP_DIR/s24-fakegit"
mkdir -p "$S24_FAKEBIN"
S24_TRANS=$(python3 -c "
import pathlib, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import build as b
import cache_schema as cs
# The walk reports INDETERMINATE while corpus_history_id still answers with a
# real tip:count -- i.e. git is healthy now, so the failure was transient.
real = b.collect_git_timestamps
def failing(repo_root, files, todo_root=None):
    ts, _ = real(repo_root, files, todo_root)
    return ts, b.PROVENANCE_INDETERMINATE
b.collect_git_timestamps = failing
sys.argv = ['build.py', '--quiet', '--root', '$S24_TREE/todo',
            '--repo-root', '$S24_TREE',
            '--output', '$S24_TREE/build/transient.json']
rc = b.main()
print(rc, pathlib.Path('$S24_TREE/build/transient.json').exists())
" 2>&1 | tail -1)
if [ "$S24_TRANS" = "3 False" ]; then
    t_pass "section 24: mutation check -- a transient timestamp-walk failure aborts the build instead of publishing null timestamps under a valid binding"
else
    t_fail "section 24: transient walk failure gave '$S24_TRANS' (want '3 False' -- rc 3 and NO cache written)"
fi
# THE SAME RULE ON THE OTHER MARKER. Gating only INDETERMINATE left the identical
# hole one door over (Codex adversarial, section 24, [high]): a corpus in a
# DIFFERENT repository than --repo-root derives no timestamp at all, and the
# fallback probe then reads the corpus's own repository and returns a real
# tip:count. A build must refuse rather than publish nulls under it. The tree
# below is a real repository reached with a --repo-root that does not contain it.
S24_OUTSIDE="$TMP_DIR/s24-outside"
mkdir -p "$S24_OUTSIDE/build"
S24_XREPO=$(python3 -c "
import pathlib, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import build as b
import cache_schema as cs
# CONTROL: the corpus really is in a repository, so the probe really does return
# a usable identity. If this is a sentinel the assertion below proves nothing.
hid = cs.corpus_history_id(pathlib.Path('$S24_TREE/todo'))
control = 'REAL' if ':' in hid and hid not in ('no-repo', 'unborn-head') else 'SENTINEL'
sys.argv = ['build.py', '--quiet', '--root', '$S24_TREE/todo',
            '--repo-root', '$S24_OUTSIDE',
            '--output', '$S24_OUTSIDE/build/cross.json']
rc = b.main()
print(control, rc, pathlib.Path('$S24_OUTSIDE/build/cross.json').exists())
" 2>&1 | tail -1)
if [ "$S24_XREPO" = "REAL 3 False" ]; then
    t_pass "section 24: a corpus outside --repo-root refuses rather than publishing null timestamps under its own repository's identity"
else
    t_fail "section 24: cross-repo build gave '$S24_XREPO' (want 'REAL 3 False')"
fi

# 24d: THE PER-VERB SPLIT COVERS THE GANTT RENDERER. `created_at` is the other
# git-derived field, and it is dereferenced in render.py -- a different module,
# behind a CLI-only command. An inventory built by grepping query.py alone
# missed it (Codex design review, section 24, [medium]), so this pins the
# selector against the CONSUMER SET rather than against a list of verb names.
S24_SEL=$(python3 -c "
import pathlib, re, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
q = pathlib.Path('$REPO_ROOT/scripts/todo-graph/query.py').read_text()
r = pathlib.Path('$REPO_ROOT/scripts/todo-graph/render.py').read_text()
# The renderer must still be a consumer; if it stops being one, this fixture is
# telling us the selector can be narrowed, not that it is wrong.
gantt_reads = 'created_at' in r
# And the selector must name it. Both the command AND the format matter: render
# in any other format consumes no timestamp.
sel = q[q.index('def _profile_for('):]
sel = sel[:sel.index('return _cs.PROFILE_QUERY\n')]
print('CONSUMES' if gantt_reads else 'NOTANYMORE',
      'SELECTED' if ('gantt' in sel and 'render' in sel) else 'MISSED',
      'PAIRED' if ('stale' in sel and 'stats' in sel) else 'INCOMPLETE')
" 2>&1)
if [ "$S24_SEL" = "CONSUMES SELECTED PAIRED" ]; then
    t_pass "section 24: the per-verb selector covers all three timestamp consumers including the Gantt renderer"
else
    t_fail "section 24: profile selector coverage gave '$S24_SEL' (want 'CONSUMES SELECTED PAIRED')"
fi

# 24e: THE TWO QUERY PROFILES DIFFER ON THE HISTORY AXIS AND NOTHING ELSE.
# They were separate literal tuples, so widening one later would silently let
# the same reader accept under `stale` a cache it rejects under `ready` (Codex
# consistency, section 24, [medium]). Pinned by EQUALITY rather than by naming
# the members, so a legitimate future widening only has to happen once.
S24_SUBTREE=$(python3 -c "
import sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
same = cs.PROFILE_QUERY.subtrees == cs.PROFILE_QUERY_HISTORY.subtrees
axis = (not cs.PROFILE_QUERY.requires_history) and cs.PROFILE_QUERY_HISTORY.requires_history
ready = cs.PROFILE_QUERY.require_stamped_population == cs.PROFILE_QUERY_HISTORY.require_stamped_population
print('SAME' if same else 'SKEWED', 'AXIS' if axis else 'NOAXIS',
      'READY' if ready else 'READYSKEW')
" 2>&1)
if [ "$S24_SUBTREE" = "SAME AXIS READY" ]; then
    t_pass "section 24: the two query profiles differ on the history axis alone"
else
    t_fail "section 24: query profile parity gave '$S24_SUBTREE' (want 'SAME AXIS READY')"
fi

# 24f: NO_DERIVATION TAKES THE WIDER RESCUE SET. A corpus in a repository whose
# history never touched it returns `no-history:0`, and null timestamps are then
# the only possible answer -- refusing it rejects a legitimate tree (Codex
# adversarial, section 24, [medium]). INDETERMINATE must NOT accept the same
# value, because the walk emits it itself on a successful empty read.
S24_RESCUE=$(python3 -c "
import sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import build as b
print('WIDER' if 'no-history:0' in b._DETERMINATE_NO_DERIVATION else 'NARROW',
      'STRICT' if 'no-history:0' not in b._DETERMINATE_NO_GIT else 'LOOSE',
      'SUPERSET' if b._DETERMINATE_NO_GIT <= b._DETERMINATE_NO_DERIVATION else 'DIVERGED')
" 2>&1)
if [ "$S24_RESCUE" = "WIDER STRICT SUPERSET" ]; then
    t_pass "section 24: no-history:0 rescues NO_DERIVATION but never INDETERMINATE"
else
    t_fail "section 24: marker rescue sets gave '$S24_RESCUE' (want 'WIDER STRICT SUPERSET')"
fi

# 24g: PRODUCER AND READER NAME THE SAME PATHS. The walk hardcoded `-- todo`
# while corpus_history_id scopes `.` from the given root, so a corpus in a
# differently-named in-repo directory refused with rc 3 and nothing concurrent
# had happened (Codex consistency, section 24, [medium]). Built with --root
# pointing at `plans`, which is the case the old code got wrong.
S24_PLANS="$TMP_DIR/s24-plans"
mkdir -p "$S24_PLANS/plans/00-infrastructure" "$S24_PLANS/build"
cp "$PW_TREE/todo/01-test/TODO-01-pw.md" "$S24_PLANS/plans/00-infrastructure/"
(
    cd "$S24_PLANS" || exit 1
    git init -q . && git config user.email "t@example.invalid" \
        && git config user.name "s24 plans fixture" \
        && git add -A && git commit -q --no-verify -m "corpus in plans/"
) >/dev/null 2>&1
S24_PATHSPEC=$(python3 -c "
import pathlib, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import build as b
sys.argv = ['build.py', '--quiet', '--root', '$S24_PLANS/plans',
            '--repo-root', '$S24_PLANS',
            '--output', '$S24_PLANS/build/todo-cache.json']
rc = b.main()
print(rc, pathlib.Path('$S24_PLANS/build/todo-cache.json').exists())
" 2>&1 | tail -1)
if [ "$S24_PATHSPEC" = "0 True" ]; then
    t_pass "section 24: a corpus in a directory not named todo/ builds instead of refusing itself"
else
    t_fail "section 24: non-default --root build gave '$S24_PATHSPEC' (want '0 True')"
fi
# 24g-magic: THE DERIVED PATHSPEC IS LITERAL. `--` ends option parsing but does
# NOT stop git interpreting pathspec magic, so a directory whose NAME is magic
# selected a different tree once the path stopped being the hardcoded `todo`
# (Codex re-adversarial, section 24, [medium]). A decoy touched by the SAME
# commits is what makes this more than a refusal: tip and count can agree while
# the emitted filenames never match, so null timestamps pass the comparison.
S24_MAGIC="$TMP_DIR/s24-magic"
mkdir -p "$S24_MAGIC/:(top)plans/00-infrastructure" "$S24_MAGIC/plans" "$S24_MAGIC/build" 2>/dev/null
if [ -d "$S24_MAGIC/:(top)plans/00-infrastructure" ]; then
    cp "$PW_TREE/todo/01-test/TODO-01-pw.md" "$S24_MAGIC/:(top)plans/00-infrastructure/"
    cp "$PW_TREE/todo/01-test/TODO-01-pw.md" "$S24_MAGIC/plans/TODO-99-decoy.md"
    (
        cd "$S24_MAGIC" || exit 1
        git init -q . && git config user.email "t@example.invalid" \
            && git config user.name "s24 magic fixture" \
            && git add -A && git commit -q --no-verify -m "both dirs in one commit"
    ) >/dev/null 2>&1
    S24_MAGIC_OUT=$(python3 -c "
import json, pathlib, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import build as b
sys.argv = ['build.py', '--quiet', '--root', '$S24_MAGIC/:(top)plans',
            '--repo-root', '$S24_MAGIC',
            '--output', '$S24_MAGIC/build/todo-cache.json']
rc = b.main()
out = pathlib.Path('$S24_MAGIC/build/todo-cache.json')
if rc != 0 or not out.exists():
    print('rc', rc, 'nocache')
else:
    nodes = json.loads(out.read_text())
    # The timestamps must be REAL: a magic-interpreted pathspec would walk the
    # decoy, emit filenames that never match, and leave these null.
    dated = [n for n in nodes if n.get('created_at') and n.get('last_active_at')]
    print('rc', rc, 'dated', len(dated), 'of', len(nodes))
" 2>&1 | tail -1)
    if [ "$S24_MAGIC_OUT" = "rc 0 dated 1 of 1" ]; then
        t_pass "section 24: a pathspec-magic corpus directory is taken literally, so its timestamps are real"
    else
        t_fail "section 24: magic-named root gave '$S24_MAGIC_OUT' (want 'rc 0 dated 1 of 1')"
    fi
else
    t_pass "section 24: pathspec-magic fixture skipped (filesystem rejects the directory name)"
fi

# 24h: DEFERRING THE HISTORY COMPARISON IS NOT SKIPPING IT. check_freshness now
# hands the RECORDED id back instead of probing, so one post-walk probe proves
# the whole property (Codex perf, section 24, [medium]). A binding with no
# history id must therefore REFUSE here rather than sail through with None and
# make the later re-check vacuous.
S24_DEFER=$(python3 -c "
import json, pathlib, sys
sys.path.insert(0, '$REPO_ROOT/scripts/todo-graph')
import cache_schema as cs
p = pathlib.Path('$S24_TREE/build/todo-cache.json')
b = p.read_bytes()
out = {}
cs.check_freshness(p, '$S24_TREE/todo', cache_bytes=b,
                   profile=cs.PROFILE_QUERY_HISTORY, history_out=out)
got = 'RECORDED' if out.get('recorded') else 'EMPTY'
# CONTROL: the deferring path must NOT have walked git. Break corpus_history_id
# and the same call must still succeed.
cs.corpus_history_id = lambda *a, **k: (_ for _ in ()).throw(AssertionError('probed'))
try:
    cs.check_freshness(p, '$S24_TREE/todo', cache_bytes=b,
                       profile=cs.PROFILE_QUERY_HISTORY, history_out={})
    noprobe = 'NOPROBE'
except AssertionError:
    noprobe = 'PROBED'
# And a binding with the history id stripped must refuse rather than defer None.
side = cs.sidecar_path(p, __import__('hashlib').sha256(b).hexdigest())
rec = json.loads(side.read_bytes()); rec.pop('history_id', None)
side.write_text(json.dumps(rec))
try:
    cs.check_freshness(p, '$S24_TREE/todo', cache_bytes=b,
                       profile=cs.PROFILE_QUERY_HISTORY, history_out={})
    closed = 'FAILOPEN'
except cs.CacheSchemaError:
    closed = 'FAILCLOSED'
print(got, noprobe, closed)
" 2>&1 | tail -1)
if [ "$S24_DEFER" = "RECORDED NOPROBE FAILCLOSED" ]; then
    t_pass "section 24: the deferring load hands back the recorded id, walks no git, and refuses a binding with none"
else
    t_fail "section 24: deferred-history load gave '$S24_DEFER' (want 'RECORDED NOPROBE FAILCLOSED')"
fi

# ======================================================================
# Section 27: replace refs and grafts cannot move a derived timestamp
# ----------------------------------------------------------------------
# The axis section 24 left open: `corpus_history_id` projects the history onto
# (tip, count), and BOTH `git replace` and the deprecated graft file can change
# what `git log` reports for an OLDER commit while leaving that pair equal. The
# cache is then certified against a history it did not come from.
#
# EVERY CASE BELOW ASSERTS THE PAIR IS UNCHANGED FIRST. Without that control the
# fixture would pass just as well against section 21's count check, and would be
# testing cardinality rather than replacement -- the exact confusion the section
# text warns about.
S27=$(python3 - "$REPO_ROOT" <<'S27EOF'
import json, os, pathlib, shutil, subprocess, sys, tempfile

repo = pathlib.Path(sys.argv[1])
tg = repo / "scripts/todo-graph"
GITENV = dict(os.environ,
              GIT_AUTHOR_NAME="s27", GIT_AUTHOR_EMAIL="s27@example.invalid",
              GIT_COMMITTER_NAME="s27", GIT_COMMITTER_EMAIL="s27@example.invalid")
out = []


git_failures = []


def git(d, *args, env_extra=None):
    """Run git in `d`. A NONZERO exit is RECORDED, never discarded.

    A silently-failing setup command (a `git replace` that did not take, a
    namespace git refused) would otherwise leave the fixture asserting
    "protected build is stable" about a repo where nothing was ever replaced
    (Codex adversarial, section 27, [medium]).
    """
    r = subprocess.run(["git", "-C", str(d), *args], capture_output=True,
                       text=True, env=dict(GITENV, **(env_extra or {})))
    if r.returncode != 0 and args[:1] != ("for-each-ref",):
        git_failures.append(f"{args[0]}:{r.returncode}")
    return r.stdout.strip()


def make_repo():
    """Three corpus-touching commits with pinned, distinct committer dates."""
    d = pathlib.Path(tempfile.mkdtemp())
    (d / "todo" / "01-x").mkdir(parents=True)
    git(d, "init", "-q", ".")
    for i in (1, 2, 3):
        (d / "todo" / "01-x" / f"TODO-0{i}-a.md").write_text(
            f"---\nschema_version: 1\nid: s27-{i}\ndomain: 01-x\nstatus: active\n"
            f'title: "A{i}"\n---\n\n# A{i}\n\n## 1. One\n\n- [ ] a thing\n')
        git(d, "add", "-A")
        subprocess.run(["git", "-C", str(d), "commit", "-q", "--no-verify",
                        "-m", f"c{i}"], capture_output=True, text=True,
                       env=dict(GITENV,
                                GIT_AUTHOR_DATE=f"2020-01-0{i} 00:00:00 +0000",
                                GIT_COMMITTER_DATE=f"2020-01-0{i} 00:00:00 +0000"))
    return d


def pair(d, env_extra=None):
    """The (tip, count) `corpus_history_id` projects onto.

    `env_extra` is not optional garnish: a case that sets up under a custom
    GIT_REPLACE_REF_BASE must MEASURE under it too, or the control describes a
    different repository than the one under test.
    """
    return (git(d, "log", "-1", "--format=%H", "--", "todo",
                env_extra=env_extra),
            git(d, "rev-list", "--count", "HEAD", "--", "todo",
                env_extra=env_extra))


def stamps(d, srcdir=None, env_extra=None):
    """created_at/last_active_at per node, via build.py (real or mutated)."""
    src = pathlib.Path(srcdir) if srcdir else tg
    cache = d / "build" / "c.json"
    cache.parent.mkdir(exist_ok=True)
    if cache.exists():
        cache.unlink()   # never let a previous run's cache answer for this one
    r = subprocess.run([sys.executable, str(src / "build.py"), "--quiet",
                        "--root", str(d / "todo"), "--repo-root", str(d),
                        "--output", str(cache)], capture_output=True, text=True,
                       env=dict(os.environ, **(env_extra or {})))
    if r.returncode != 0 or not cache.exists():
        return None
    return sorted((n["id"], n.get("created_at"), n.get("last_active_at"))
                  for n in json.loads(cache.read_text()))


def mutated_tree(patch):
    """A copy of the todo-graph module tree with `patch` applied to cache_schema.

    build.py puts its OWN directory at sys.path[0], so patching a copy is the
    only way to make it import a modified cache_schema. The WHOLE tree is copied
    rather than the two files: build.py also resolves `schema/cache.schema.json`
    and its sibling modules against that same directory, and a two-file copy
    dies at import with FileNotFoundError -- which the mutation leg would then
    report as "no drift", i.e. as a PASS of the thing it exists to disprove.
    """
    m = pathlib.Path(tempfile.mkdtemp()) / "tg"
    shutil.copytree(tg, m,
                    ignore=shutil.ignore_patterns("tests", "__pycache__"))
    text = (m / "cache_schema.py").read_text()
    old, new = patch
    if text.count(old) != 1:
        return None
    (m / "cache_schema.py").write_text(text.replace(old, new, 1))
    return m


def replace_oldest(d, env_extra=None):
    """Replace the oldest corpus commit: same tree, same parents, new date."""
    old = git(d, "rev-list", "--max-parents=0", "HEAD")
    tree = git(d, "show", "-s", "--format=%T", old)
    new = subprocess.run(
        ["git", "-C", str(d), "commit-tree", tree, "-m", "c1"],
        capture_output=True, text=True,
        env=dict(GITENV, GIT_AUTHOR_DATE="2021-06-06 06:06:06 +0000",
                 GIT_COMMITTER_DATE="2021-06-06 06:06:06 +0000")).stdout.strip()
    git(d, "replace", "-f", old, new, env_extra=env_extra)


# -- 27a: a replace ref cannot move a derived timestamp -----------------
d = make_repo()
before_pair, before = pair(d), stamps(d)
if before is None:
    out.append("A-CONTROL-NO-CACHE")
else:
    replace_oldest(d)
    after_pair, after = pair(d), stamps(d)
    if after_pair != before_pair:
        # The replacement moved the pair, so section 21's count/tip check would
        # already catch it and this fixture proves nothing about section 27.
        out.append("A-CONTROL-PAIR-MOVED")
    elif before is None or after is None:
        out.append("A-BUILD-FAILED")
    else:
        out.append("A-STABLE" if after == before else "A-DRIFTED")

# -- 27a-mutation: revert the mechanism, the drift must appear ----------
mut = mutated_tree(('HISTORY_GIT_GLOBALS = ("--no-replace-objects",)',
                    "HISTORY_GIT_GLOBALS = ()"))
if mut is None:
    out.append("AM-NEEDLE-MISS")
else:
    m_after = stamps(d, mut)
    # A mutant that fails to BUILD produces no drift, which is indistinguishable
    # from a mutant the mechanism defeated unless it is named separately. It was
    # exactly that collapse (a two-file copy dying on a missing schema/) that
    # made this leg first report a false pass.
    out.append("AM-BUILD-FAILED" if m_after is None else
               "AM-DRIFTED" if m_after != before else "AM-STABLE")

# -- 27b: a replacement in a NON-DEFAULT namespace is still inert -------
# This is why a digest of `refs/replace` was rejected as the mechanism: git
# honours GIT_REPLACE_REF_BASE, and such a digest reads an EMPTY set while the
# walk is replaced (Codex design review, section 27, [high]).
#
# EVERY MEASUREMENT BELOW RUNS UNDER THE CUSTOM BASE, and the leg proves its own
# control fires by first showing an UNPROTECTED build DRIFTS there. Without that
# the case passes vacuously: if git ignored the namespace, or `git replace`
# failed, "protected build is stable" would be true for the wrong reason
# (Codex adversarial, section 27, [medium]).
BASE = {"GIT_REPLACE_REF_BASE": "refs/myreplace/"}
d2 = make_repo()
b2_pair, b2 = pair(d2, BASE), stamps(d2, env_extra=BASE)
replace_oldest(d2, BASE)
if git_failures:
    out.append("B-CONTROL-GIT-SETUP-FAILED:" + ",".join(git_failures))
else:
    # A digest of refs/replace -- candidate B's mechanism -- sees nothing here.
    digest_would_see = git(d2, "for-each-ref", "--format=%(refname)",
                           "refs/replace", env_extra=BASE)
    created = git(d2, "for-each-ref", "--format=%(refname)", "refs/myreplace/",
                  env_extra=BASE)
    # CONTROL: an unprotected walk under this base MUST observe the replacement.
    unprot = mutated_tree(('HISTORY_GIT_GLOBALS = ("--no-replace-objects",)',
                           "HISTORY_GIT_GLOBALS = ()"))
    u2 = stamps(d2, unprot, env_extra=BASE) if unprot else None
    a2_pair, a2 = pair(d2, BASE), stamps(d2, env_extra=BASE)
    if created == "":
        out.append("B-CONTROL-NO-REPLACE-REF-CREATED")
    elif digest_would_see != "":
        out.append("B-CONTROL-REFS-REPLACE-NONEMPTY")
    elif u2 is None or u2 == b2:
        # git ignored the custom namespace, or the mutant did not build: either
        # way the protected result below would mean nothing.
        out.append("B-CONTROL-UNPROTECTED-DID-NOT-DRIFT")
    elif a2_pair != b2_pair:
        out.append("B-CONTROL-PAIR-MOVED")
    else:
        out.append("B-STABLE" if a2 is not None and a2 == b2 else "B-DRIFTED")

# -- 27c: an EQUAL-TIP, EQUAL-COUNT graft is inert ----------------------
# THE PAIR MUST SURVIVE THE GRAFT, which is the whole point of the case. An
# earlier version grafted the tip onto the root, which CHANGES the count -- so
# it proved only that a graft section 21's count already catches is neutralised,
# not the hidden case this section exists for (Codex adversarial round 2,
# section 27, [medium]).
#
# Construction: three corpus commits C1,C2,C3 plus an independent root S that
# also touches the corpus with an OLDER date. Reparenting C2 onto S leaves the
# tip and the corpus-touching count at 3 while the OLDEST corpus commit becomes
# S -- so `created_at` moves under an identity that compares equal.
d3 = make_repo()
b3_pair, b3 = pair(d3), stamps(d3)
c2 = git(d3, "rev-parse", "HEAD~1")
# S is built through the index, which is then restored to HEAD.
zz = d3 / "zz.tmp"
zz.write_text("independent root\n")
blob = git(d3, "hash-object", "-w", str(zz))
git(d3, "read-tree", "--empty")
git(d3, "update-index", "--add", "--cacheinfo",
    f"100644,{blob},todo/01-x/TODO-01-a.md")
s_tree = git(d3, "write-tree")
s = subprocess.run(["git", "-C", str(d3), "commit-tree", s_tree, "-m", "S"],
                   capture_output=True, text=True,
                   env=dict(GITENV,
                            GIT_AUTHOR_DATE="2019-01-01 00:00:00 +0000",
                            GIT_COMMITTER_DATE="2019-01-01 00:00:00 +0000")
                   ).stdout.strip()
git(d3, "read-tree", "HEAD")
zz.unlink()
(d3 / ".git" / "info").mkdir(parents=True, exist_ok=True)
(d3 / ".git" / "info" / "grafts").write_text(f"{c2} {s}\n")
a3_pair = pair(d3)
# CONTROL 1: the graft must be LIVE -- an unprotected walk must see it.
ungrafted_walk = git(d3, "log", "--format=%ct", "--reverse", "--", "todo",
                     env_extra={"GIT_GRAFT_FILE": os.devnull})
grafted_walk = git(d3, "log", "--format=%ct", "--reverse", "--", "todo")
if git_failures:
    out.append("C-CONTROL-GIT-SETUP-FAILED:" + ",".join(git_failures))
elif grafted_walk == ungrafted_walk:
    out.append("C-CONTROL-GRAFT-INERT")
# CONTROL 2: and it must be the HIDDEN shape -- the pair unchanged. Without
# this the case collapses back into section 21's count check.
elif a3_pair != b3_pair:
    out.append(f"C-CONTROL-PAIR-MOVED({b3_pair[1]}->{a3_pair[1]})")
else:
    a3 = stamps(d3)
    out.append("C-STABLE" if a3 is not None and a3 == b3 else "C-DRIFTED")

# -- 27c-mutation: drop GIT_GRAFT_FILE, the graft must bite -------------
mut3 = mutated_tree(('    env["GIT_GRAFT_FILE"] = os.devnull\n', ""))
if mut3 is None:
    out.append("CM-NEEDLE-MISS")
else:
    m3 = stamps(d3, mut3)
    out.append("CM-BUILD-FAILED" if m3 is None else
               "CM-DRIFTED" if m3 != b3 else "CM-STABLE")

print(" ".join(out))
S27EOF
)
S27_WANT="A-STABLE AM-DRIFTED B-STABLE CM-DRIFTED"
S27_GOT="$S27"
case "$S27_GOT" in
    "A-STABLE AM-DRIFTED B-STABLE C-STABLE CM-DRIFTED")
        t_pass "section 27: replace refs (default and custom namespace) and grafts cannot move a derived timestamp"
        t_pass "section 27: mutation check -- reverting --no-replace-objects or GIT_GRAFT_FILE lets the history move the timestamps"
        ;;
    *)
        t_fail "section 27: history-neutralisation gave '$S27_GOT' (want 'A-STABLE AM-DRIFTED B-STABLE C-STABLE CM-DRIFTED'; $S27_WANT are the load-bearing legs)"
        ;;
esac

# ----------------------------------------------------------------------
# Section 29: repair mode fails VISIBLY on what it could not resolve
#
# Before this section `--fix-line-numbers` counted its unresolvable targets and
# threw the count away (`main()` unpacked it as `_unres`), and a target that
# resolved to a file NOT containing the named item was skipped in silence at
# `len(hits) == 0`. Both left a stale line number behind and exited 0. Measured
# on the live corpus while implementing: 50 unresolvable targets and 75 missing
# items, none of which produced a single line of output under the `--quiet` the
# sole caller uses.
#
# Every fixture below asserts the EXIT CODE, the DIAGNOSTIC, and that the stale
# stamp was preserved rather than invented over -- a rc alone would pass on a
# tool that repaired the wrong thing loudly.
# ----------------------------------------------------------------------

# s29_tree <root> <holder-stamp-block> [extra-file-basename] [extra-title]
# Writes a two-file corpus: TODO-02-h.md carries the stamp(s) under test and
# TODO-03-t.md is the target holding the item they name.
s29_tree() {
    local root="$1" stamps="$2"
    rm -rf "$root"
    mkdir -p "$root/todo/01-test"
    s29_file "$root/todo/01-test/TODO-02-h.md" "fm-s29-h" "TODO-02 -- s29 stamp holder" "$stamps"
    s29_file "$root/todo/01-test/TODO-03-t.md" "fm-s29-t" "TODO-03 -- s29 item holder" ""
}

# s29_file <path> <id> <title> <trailer>
s29_file() {
    local path="$1" fid="$2" title="$3" trailer="$4"
    cat > "$path" <<S29FEOF
---
schema_version: 1
id: $fid
domain: 01-test
status: active
title: "$title"
---
# $title
## Inputs
| Path | Purpose |
| ---- | ------- |
| \`src/foo\` | example |
## Outcome
Body.
## Implementation Order
| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | 1 | First | -- | [x] |
## 1. First Section
Body.
- [ ] Wire the resolver
- [x] Commit
$trailer
S29FEOF
}

# s29_run <root> [python-file] -- build the cache then repair; echoes the rc.
s29_run() {
    local root="$1" py="${2:-$VALIDATE_PY}"
    timeout 60 python3 "$BUILD_PY" --quiet --root "$root/todo" \
        --output "$root/cache.json" --repo-root "$root" >/dev/null 2>&1
    timeout 60 python3 "$py" --cache "$root/cache.json" --repo-root "$root" \
        --fix-line-numbers --write --quiet > "$root/out.txt" 2>&1
    echo $?
}

# s29_run_dry <root> -- the same, in the DEFAULT dry-run mode (no --write).
# The exit contract is a property of repair mode, not of --write, and every
# fixture here used to pass --write (Codex test-coverage, section 29, [medium]).
s29_run_dry() {
    local root="$1"
    timeout 60 python3 "$BUILD_PY" --quiet --root "$root/todo" \
        --output "$root/cache.json" --repo-root "$root" >/dev/null 2>&1
    timeout 60 python3 "$VALIDATE_PY" --cache "$root/cache.json" --repo-root "$root" \
        --fix-line-numbers --quiet > "$root/dry.txt" 2>&1
    echo $?
}

# Sub-test 29a: CONTROL -- a corpus with nothing wrong still exits 0. Without
# this the rc-1 assertions below could all pass on a tool that returns 1
# unconditionally.
S29A="$TMP_DIR/s29-clean"
s29_tree "$S29A" '> **Accepted:** ok -> XREF: TODO-03-t.md (item: "Wire the resolver" at line 999)'
S29A_RC=$(s29_run "$S29A")
S29A_LEFT=$(grep -c 'at line 999' "$S29A/todo/01-test/TODO-02-h.md")
if [ "$S29A_RC" = "0" ] && [ "$S29A_LEFT" = "0" ]; then
    t_pass "section 29: CONTROL a fully repairable corpus exits 0 and rewrites the stamp"
else
    t_fail "section 29: clean-corpus control rc=$S29A_RC stale_left=$S29A_LEFT (want 0/0)"
fi

# Sub-test 29b: an unresolvable XREF target exits 1, NAMES the target, and
# leaves the stored line number alone.
S29B="$TMP_DIR/s29-unresolvable"
s29_tree "$S29B" '> **Accepted:** bad -> XREF: TODO-77-does-not-exist.md (item: "Wire the resolver" at line 999)'
S29B_RC=$(s29_run "$S29B")
S29B_LEFT=$(grep -c 'at line 999' "$S29B/todo/01-test/TODO-02-h.md")
if [ "$S29B_RC" = "1" ] && grep -q "unresolvable XREF target" "$S29B/out.txt" \
    && grep -q "TODO-77-does-not-exist" "$S29B/out.txt" && [ "$S29B_LEFT" = "1" ]; then
    t_pass "section 29: an unresolvable target exits 1, names the target, and preserves the stale line"
else
    t_fail "section 29: unresolvable rc=$S29B_RC stale_left=$S29B_LEFT out=$(cat "$S29B/out.txt")"
fi

# Sub-test 29c: the target RESOLVES and does not contain the named item. This
# is the second silent path (Codex design review, section 29, [high]): the file
# was found, so the unresolvable counter never moved, and `len(hits) == 0` fell
# through to `continue` without a word.
S29C="$TMP_DIR/s29-missing-item"
s29_tree "$S29C" '> **Accepted:** gone -> XREF: TODO-03-t.md (item: "Item that was renamed away" at line 999)'
S29C_RC=$(s29_run "$S29C")
S29C_LEFT=$(grep -c 'at line 999' "$S29C/todo/01-test/TODO-02-h.md")
if [ "$S29C_RC" = "1" ] && grep -q "not found in" "$S29C/out.txt" \
    && grep -q "Item that was renamed away" "$S29C/out.txt" && [ "$S29C_LEFT" = "1" ]; then
    t_pass "section 29: a resolved target missing the named item exits 1 and names the item"
else
    t_fail "section 29: missing-item rc=$S29C_RC stale_left=$S29C_LEFT out=$(cat "$S29C/out.txt")"
fi

# Sub-test 29d: rc 1 is a REPORT, not a rollback. A corpus carrying one
# repairable stamp and one unrepairable one must still apply the safe rewrite
# AND still exit non-zero. A tool that rolled the good one back would also
# "pass" an rc-only assertion.
S29D="$TMP_DIR/s29-partial"
s29_tree "$S29D" '> **Accepted:** good -> XREF: TODO-03-t.md (item: "Wire the resolver" at line 999)
> **Accepted:** gone -> XREF: TODO-03-t.md (item: "Item that was renamed away" at line 888)'
S29D_RC=$(s29_run "$S29D")
S29D_GOOD=$(grep -c 'at line 999' "$S29D/todo/01-test/TODO-02-h.md")
S29D_BAD=$(grep -c 'at line 888' "$S29D/todo/01-test/TODO-02-h.md")
if [ "$S29D_RC" = "1" ] && [ "$S29D_GOOD" = "0" ] && [ "$S29D_BAD" = "1" ]; then
    t_pass "section 29: a partial repair applies the safe rewrite and still exits 1"
else
    t_fail "section 29: partial rc=$S29D_RC good_rewritten=$S29D_GOOD bad_preserved=$S29D_BAD"
fi

# Sub-test 29e: a corpus whose (directory, number) resolver keys collide is
# refused UP FRONT at rc 2, with nothing written -- and it is refused for all
# FOUR spellings of the colliding target, so repair-mode safety cannot come to
# depend on surface form again.
#
# MEASURED while implementing (all three states on this exact tree):
#   pre-section-29     rc 0, ZERO lines of output, all 4 stamps left stale
#   refusal reverted   rc 1 and four per-stamp `unresolvable` lines -- section
#                      26's resolver already fails closed on every spelling, so
#                      nothing is mis-rewritten; what is missing is the DIAGNOSIS
#   as shipped         rc 2, one line naming the colliding pair, nothing written
# That is why this refusal is worth its lines even though section 26 made the
# corpus safe: it turns N confusing per-stamp errors into one true statement.
# THE FIFTH STAMP IS LOAD-BEARING. With only the four colliding stamps, none is
# repairable, so moving the refusal to AFTER fix_line_numbers() would produce
# the same rc, the same message, the same hash and the same stale count -- the
# fixture could not see the ordering its comment claims (Codex test-coverage,
# section 29, [high]). The `safe` stamp below resolves cleanly to TODO-03, so it
# WOULD be rewritten by a late-refusing build; asserting it is still stale at
# rc 2 is what pins "refused before touching anything".
S29E="$TMP_DIR/s29-collision"
rm -rf "$S29E"; mkdir -p "$S29E/todo/01-test"
s29_file "$S29E/todo/01-test/TODO-01-a.md" "fm-s29-a" "TODO-01 -- s29 collide a" ""
s29_file "$S29E/todo/01-test/TODO-01-dup.md" "fm-s29-d" "TODO-01 -- s29 collide dup" ""
s29_file "$S29E/todo/01-test/TODO-03-t.md" "fm-s29-et" "TODO-03 -- s29 item holder" ""
s29_file "$S29E/todo/01-test/TODO-02-h.md" "fm-s29-eh" "TODO-02 -- s29 collision holder" \
'> **Accepted:** q -> XREF: [`TODO-01`](01-test/TODO-01-a.md) (item: "Wire the resolver" at line 999)
> **Accepted:** b -> XREF: TODO-01 (item: "Wire the resolver" at line 999)
> **Accepted:** r -> XREF: TODO-01-a.md (item: "Wire the resolver" at line 999)
> **Accepted:** f -> XREF: todo/01-test/TODO-01-a.md (item: "Wire the resolver" at line 999)
> **Accepted:** safe -> XREF: TODO-03-t.md (item: "Wire the resolver" at line 777)'
# Hash the WHOLE tree, not just the holder: a late refusal could touch any file.
S29E_BEFORE=$(find "$S29E/todo" -type f -name '*.md' | sort | xargs md5sum | md5sum | cut -d' ' -f1)
S29E_RC=$(s29_run "$S29E")
S29E_AFTER=$(find "$S29E/todo" -type f -name '*.md' | sort | xargs md5sum | md5sum | cut -d' ' -f1)
S29E_LEFT=$(grep -c 'at line 999' "$S29E/todo/01-test/TODO-02-h.md")
S29E_SAFE=$(grep -c 'at line 777' "$S29E/todo/01-test/TODO-02-h.md")
if [ "$S29E_RC" = "2" ] && grep -q "resolver keys collide" "$S29E/out.txt" \
    && [ "$S29E_BEFORE" = "$S29E_AFTER" ] && [ "$S29E_LEFT" = "4" ] \
    && [ "$S29E_SAFE" = "1" ]; then
    t_pass "section 29: a resolver-key collision refuses up front at rc 2 across all four target spellings, leaving even an independently repairable stamp untouched"
else
    t_fail "section 29: collision rc=$S29E_RC unchanged=$([ "$S29E_BEFORE" = "$S29E_AFTER" ] && echo yes || echo no) spellings_left=$S29E_LEFT safe_untouched=$S29E_SAFE out=$(cat "$S29E/out.txt")"
fi

# Sub-test 29e2: ORDERING MUTATION -- move the collision refusal to AFTER
# fix_line_numbers() and the `safe` stamp above must get rewritten. This is the
# control that makes 29e's ordering claim falsifiable; without it the refusal
# could sit anywhere before the return and nothing would notice.
S29G="$TMP_DIR/s29-order-mutation"
mkdir -p "$S29G"
python3 - "$VALIDATE_PY" "$S29G/validate_late.py" <<'S29GEOF'
import sys
src = open(sys.argv[1], encoding="utf-8").read()
call = "        key_collisions = check_duplicate_resolver_key(nodes)\n"
# STRUCTURE, NOT PROSE. An earlier version pinned the refusal's exact wording
# and broke the moment the message was corrected -- failing closed, which is
# right, but for a reason that has nothing to do with ordering. Assert only
# that the block still guards on the collision and refuses.
start = src.index(call)
end = src.index("        _updates, ambig, unres, missing, report = fix_line_numbers(", start)
block = src[start:end]
assert "if key_collisions:" in block and "_refuse(" in block, \
    "collision guard not found in the expected block"
rest = src[end:]
anchor = rest.index("\n", rest.index("        )\n")) + 1
open(sys.argv[2], "w", encoding="utf-8").write(
    src[:start] + rest[:anchor] + block + rest[anchor:]
)
print("LATE-MUTATION-APPLIED")
S29GEOF
cp "$REPO_ROOT/scripts/todo-graph/cache_schema.py" "$S29G/cache_schema.py"
if [ ! -f "$S29G/validate_late.py" ]; then
    t_fail "section 29: ordering mutation could not be built -- the refusal block moved, update this fixture"
else
    S29G_TREE="$TMP_DIR/s29-order-tree"
    rm -rf "$S29G_TREE"; mkdir -p "$S29G_TREE/todo/01-test"
    cp "$S29E/todo/01-test/"*.md "$S29G_TREE/todo/01-test/"
    S29G_RC=$(s29_run "$S29G_TREE" "$S29G/validate_late.py")
    S29G_SAFE=$(grep -c 'at line 777' "$S29G_TREE/todo/01-test/TODO-02-h.md")
    if [ "$S29G_SAFE" = "0" ]; then
        t_pass "section 29: ORDERING MUTATION refusing after the repair rewrites the safe stamp, which 29e forbids"
    else
        t_fail "section 29: ordering mutation left the safe stamp stale (rc=$S29G_RC safe_left=$S29G_SAFE) -- 29e cannot detect a late refusal"
    fi
fi

# Sub-test 29f: MUTATION -- revert the up-front refusal and the same corpus must
# stop refusing. Pinned to rc 1 (not merely "not 2"): section 26's resolver
# still fails closed, so the mutant degrades to per-stamp reports rather than to
# a wrong rewrite, and asserting that keeps the two layers honestly separated.
S29F="$TMP_DIR/s29-mutation"
mkdir -p "$S29F"
sed 's/^        key_collisions = check_duplicate_resolver_key(nodes)$/        key_collisions = []  # MUTATION/' \
    "$VALIDATE_PY" > "$S29F/validate_mut.py"
cp "$REPO_ROOT/scripts/todo-graph/cache_schema.py" "$S29F/cache_schema.py"
if ! grep -q "# MUTATION" "$S29F/validate_mut.py"; then
    t_fail "section 29: mutation could not be applied -- the refusal line moved, update this fixture"
else
    S29F_TREE="$TMP_DIR/s29-mutation-tree"
    rm -rf "$S29F_TREE"; mkdir -p "$S29F_TREE/todo/01-test"
    cp "$S29E/todo/01-test/"*.md "$S29F_TREE/todo/01-test/"
    S29F_RC=$(s29_run "$S29F_TREE" "$S29F/validate_mut.py")
    S29F_UNRES=$(grep -c "unresolvable XREF target" "$S29F_TREE/out.txt")
    if [ "$S29F_RC" = "1" ] && [ "$S29F_UNRES" = "4" ] \
        && ! grep -q "resolver keys collide" "$S29F_TREE/out.txt"; then
        t_pass "section 29: MUTATION reverting the refusal drops rc 2 to four per-stamp rc-1 reports"
    else
        t_fail "section 29: mutation rc=$S29F_RC unresolvable_lines=$S29F_UNRES (want 1/4)"
    fi
fi

# Sub-test 29h: the exit contract holds in DEFAULT DRY-RUN mode too, and a dry
# run still touches nothing. rc is a statement about the CORPUS, not about
# whether this particular invocation was allowed to write.
S29H_OK=1
S29H_DETAIL=""
for S29H_CASE in "clean:0" "unresolvable:1" "missing:1"; do
    S29H_NAME="${S29H_CASE%%:*}"; S29H_WANT="${S29H_CASE##*:}"
    S29H="$TMP_DIR/s29-dry-$S29H_NAME"
    case "$S29H_NAME" in
        clean) s29_tree "$S29H" '> **Accepted:** ok -> XREF: TODO-03-t.md (item: "Wire the resolver" at line 999)' ;;
        unresolvable) s29_tree "$S29H" '> **Accepted:** bad -> XREF: TODO-77-does-not-exist.md (item: "Wire the resolver" at line 999)' ;;
        missing) s29_tree "$S29H" '> **Accepted:** gone -> XREF: TODO-03-t.md (item: "Item that was renamed away" at line 999)' ;;
    esac
    S29H_HASH_BEFORE=$(md5sum "$S29H/todo/01-test/TODO-02-h.md" | cut -d' ' -f1)
    S29H_RC=$(s29_run_dry "$S29H")
    S29H_HASH_AFTER=$(md5sum "$S29H/todo/01-test/TODO-02-h.md" | cut -d' ' -f1)
    if [ "$S29H_RC" != "$S29H_WANT" ]; then
        S29H_OK=0; S29H_DETAIL="$S29H_DETAIL $S29H_NAME(rc=$S29H_RC want=$S29H_WANT)"
    fi
    if [ "$S29H_HASH_BEFORE" != "$S29H_HASH_AFTER" ]; then
        S29H_OK=0; S29H_DETAIL="$S29H_DETAIL $S29H_NAME(MUTATED-IN-DRY-RUN)"
    fi
done
if [ "$S29H_OK" = "1" ]; then
    t_pass "section 29: the rc 0/1 contract holds in default dry-run mode, and dry-run writes nothing"
else
    t_fail "section 29: dry-run contract broken --$S29H_DETAIL"
fi

# Sub-test 29i: ambiguity -- the THIRD incomplete-repair class -- must also
# reach the --quiet caller as a per-instance line naming the item, the target
# file, the stamp and the matching lines. The pre-existing fixture 8g greps for
# `ambiguous item_name`, which the AGGREGATE line ("N ambiguous item_name(s)
# refused") also contains, so deleting the per-instance append left 8g green
# (Codex test-coverage, section 29, [medium]). This asserts the parts only the
# per-instance line carries.
S29I="$TMP_DIR/s29-ambiguous"
rm -rf "$S29I"; mkdir -p "$S29I/todo/01-test"
s29_file "$S29I/todo/01-test/TODO-03-t.md" "fm-s29-it" "TODO-03 -- s29 ambiguous target" \
'- [ ] Wire the resolver'
s29_file "$S29I/todo/01-test/TODO-02-h.md" "fm-s29-ih" "TODO-02 -- s29 ambiguity holder" \
'> **Accepted:** amb -> XREF: TODO-03-t.md (item: "Wire the resolver" at line 999)'
S29I_RC=$(s29_run "$S29I")
S29I_LINE=$(grep "FAIL fix-line-numbers: ambiguous item_name" "$S29I/out.txt" | head -1)
S29I_LEFT=$(grep -c 'at line 999' "$S29I/todo/01-test/TODO-02-h.md")
if [ "$S29I_RC" = "1" ] && [ -n "$S29I_LINE" ] \
    && echo "$S29I_LINE" | grep -q "Wire the resolver" \
    && echo "$S29I_LINE" | grep -q "TODO-03-t.md" \
    && echo "$S29I_LINE" | grep -q "TODO-02-h.md" \
    && echo "$S29I_LINE" | grep -qE "matches lines \[[0-9]+, [0-9]+" \
    && [ "$S29I_LEFT" = "1" ]; then
    t_pass "section 29: ambiguity reaches the quiet caller naming item, target, stamp and every matching line"
else
    t_fail "section 29: quiet ambiguity rc=$S29I_RC stale_left=$S29I_LEFT line='$S29I_LINE'"
fi

# Sub-test 29j: MUTATION for 29i -- delete the per-instance ambiguity append and
# 29i's assertions must stop holding, proving they test the line and not the
# aggregate.
S29J="$TMP_DIR/s29-amb-mutation"
mkdir -p "$S29J"
python3 - "$VALIDATE_PY" "$S29J/validate_noamb.py" <<'S29JEOF'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
# Match the per-instance ambiguity emission whatever helper carries it, so a
# change of buffering mechanism does not masquerade as a missing mutation.
pat = re.compile(
    r"                    _report_diag\([^\n]*\n"
    r"                        f\"\[validate\.py\] FAIL fix-line-numbers: ambiguous item_name \"\n"
    r".*?\n                    \)\n", re.S)
out, n = pat.subn("", src, count=1)
assert n == 1, "ambiguity per-instance emission not found"
open(sys.argv[2], "w", encoding="utf-8").write(out)
S29JEOF
cp "$REPO_ROOT/scripts/todo-graph/cache_schema.py" "$S29J/cache_schema.py"
if [ ! -f "$S29J/validate_noamb.py" ]; then
    t_fail "section 29: ambiguity mutation could not be built -- the append moved, update this fixture"
else
    S29J_TREE="$TMP_DIR/s29-amb-mut-tree"
    rm -rf "$S29J_TREE"; mkdir -p "$S29J_TREE/todo/01-test"
    cp "$S29I/todo/01-test/"*.md "$S29J_TREE/todo/01-test/"
    S29J_RC=$(s29_run "$S29J_TREE" "$S29J/validate_noamb.py")
    if ! grep -q "FAIL fix-line-numbers: ambiguous item_name" "$S29J_TREE/out.txt"; then
        t_pass "section 29: MUTATION deleting the per-instance ambiguity line is caught by 29i"
    else
        t_fail "section 29: ambiguity mutation still emitted the per-instance line (rc=$S29J_RC)"
    fi
fi

# Sub-test 29k: the per-instance diagnostic listing is BUDGETED while the
# totals stay exact. Codex perf review measured one size-valid file with
# 762,597 unresolvable clauses buffering 126,591,102 characters and taking peak
# RSS from 70 MiB to 230 MiB; `_diag` bounds each FIELD and nothing bounded the
# entry COUNT. 600 clauses here is well over the 200 cap and still fast.
S29K="$TMP_DIR/s29-diag-budget"
rm -rf "$S29K"; mkdir -p "$S29K/todo/01-test"
s29_file "$S29K/todo/01-test/TODO-03-t.md" "fm-s29-kt" "TODO-03 -- s29 budget target" ""
{
    printf '> **Accepted:** many'
    S29K_I=0
    while [ "$S29K_I" -lt 600 ]; do
        printf ' -> XREF: TODO-77-does-not-exist.md (item: "gone%d" at line 999)' "$S29K_I"
        S29K_I=$((S29K_I + 1))
    done
    printf '\n'
} > "$S29K/stamp.txt"
s29_file "$S29K/todo/01-test/TODO-02-h.md" "fm-s29-kh" "TODO-02 -- s29 budget holder" "$(cat "$S29K/stamp.txt")"
S29K_RC=$(s29_run "$S29K")
S29K_LINES=$(grep -c "FAIL fix-line-numbers: unresolvable" "$S29K/out.txt")
# The TOTAL must survive truncation, and it must survive --quiet: the
# suppression line points at the summary, and the summary used to be
# quiet-gated, so under the only automatic caller the promised totals were
# never printed. Assert the exact count is present in a --quiet run.
S29K_TOTAL=$(grep -oE '[0-9]+ unresolvable target\(s\)' "$S29K/out.txt" | grep -oE '^[0-9]+' | head -1)
# EXACT, not `-le`. A loose bound passes an off-by-one cap, and counting the
# suppression LINES (not merely grepping for one) is what stops a helper that
# emits a suppression line per post-cap diagnostic -- which would satisfy a
# `grep -q` while restoring the O(n) report growth this fixture exists to
# forbid (Codex re-adversarial, section 29, [medium]).
S29K_SUPP=$(grep -c "further unresolvable-target diagnostics suppressed" "$S29K/out.txt")
if [ "$S29K_RC" = "1" ] && [ "$S29K_LINES" = "100" ] && [ "$S29K_SUPP" = "1" ] \
    && [ "$S29K_TOTAL" = "600" ]; then
    t_pass "section 29: exactly 100 per-class diagnostics and exactly one suppression line, with --quiet still reporting the exact total of $S29K_TOTAL"
else
    t_fail "section 29: diagnostic budget rc=$S29K_RC listed=$S29K_LINES supp=$S29K_SUPP total=$S29K_TOTAL (want rc 1, exactly 100, exactly 1, total 600)"
fi

# Sub-test 29l: a FLOOD OF ONE CLASS MUST NOT STARVE ANOTHER. Measured on the
# live corpus with a single shared budget: 403 pre-existing ambiguity findings
# came first in corpus order and consumed the whole allowance, leaving 0 of 50
# unresolvable and 15 of 75 missing listed -- the budget silenced exactly the
# two classes this section added. Per-class budgets are what prevent that.
S29L="$TMP_DIR/s29-class-starvation"
rm -rf "$S29L"; mkdir -p "$S29L/todo/01-test"
s29_file "$S29L/todo/01-test/TODO-03-t.md" "fm-s29-lt" "TODO-03 -- s29 starvation target" \
'- [ ] Wire the resolver'
{
    printf '> **Accepted:** flood'
    S29L_I=0
    while [ "$S29L_I" -lt 300 ]; do
        printf ' -> XREF: TODO-03-t.md (item: "Wire the resolver" at line 999)'
        S29L_I=$((S29L_I + 1))
    done
    # BOTH rare classes, not just one. Checking only the unresolvable class
    # left the missing-item class free to be starved by the same flood.
    printf ' -> XREF: TODO-77-does-not-exist.md (item: "rare" at line 888)'
    printf ' -> XREF: TODO-03-t.md (item: "Item that was renamed away" at line 777)\n'
} > "$S29L/stamp.txt"
s29_file "$S29L/todo/01-test/TODO-02-h.md" "fm-s29-lh" "TODO-02 -- s29 starvation holder" "$(cat "$S29L/stamp.txt")"
S29L_RC=$(s29_run "$S29L")
S29L_UNRES=$(grep -c "unresolvable XREF target" "$S29L/out.txt")
S29L_MISS=$(grep -c "FAIL fix-line-numbers: item .* not found in" "$S29L/out.txt")
S29L_SUM=$(grep -c "1 unresolvable target(s), 1 missing item(s)" "$S29L/out.txt")
if [ "$S29L_RC" = "1" ] && [ "$S29L_UNRES" = "1" ] && [ "$S29L_MISS" = "1" ] \
    && [ "$S29L_SUM" = "1" ]; then
    t_pass "section 29: 300 ambiguity findings starve neither the unresolvable nor the missing-item class, and the exact totals still print"
else
    t_fail "section 29: class starvation rc=$S29L_RC unres=$S29L_UNRES missing=$S29L_MISS summary=$S29L_SUM (want 1/1/1/1)"
fi

# ----------------------------------------------------------------------
# ----------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------
tb_finish sections_a
