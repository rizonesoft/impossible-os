#!/usr/bin/env python3
"""Self-test for the boot-reliability gate -- TODO-28 S2.

Exercises the PURE verdict logic (classify_boot / aggregate_runs / build_result
/ build_reliability) with canned serial fixtures and schema validation -- no
live VM, mirroring the WSL no-QEMU constraint (same pattern as test_lint.py).
Run by scripts/test-tooling.sh. Exits 0 only if every assertion holds.
"""
import sys
import os
import json

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import boot_reliability as br  # noqa: E402

try:
    import jsonschema
except ImportError:
    sys.stderr.write("boot-reliability test: jsonschema required\n")
    sys.exit(2)

PASS = 0
FAIL = 0


def check(name, cond):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  [PASS] {name}")
    else:
        FAIL += 1
        print(f"  [FAIL] {name}")


# Canonical good boot log: PASS marker + shell prompt + both required signals.
GOOD = ("[BOOT] ExitBootServices OK\n[PHASE0] BOOT_INFO magic ok\n"
        "...\nBoot complete in 1.71s\nC:\\>")

# ---- classify_boot ----
st, _ = br.classify_boot(GOOD, 0, False)
check("clean boot -> pass", st == "pass")

# FAIL precedence: a panic anywhere outweighs the PASS markers also present.
st, why = br.classify_boot(GOOD + "\nKERNEL PANIC: x", 0, False)
check("fail marker beats present PASS markers", st == "fail" and "KERNEL PANIC" in why)

st, why = br.classify_boot(GOOD + "\n[WARN] mmap: carve at cap", 0, False)
check("forbidden absent-marker -> fail", st == "fail" and "forbidden" in why)

st, _ = br.classify_boot(GOOD, 0, True)
check("timeout -> fail even with good log", st == "fail")

st, why = br.classify_boot("Boot complete in 1.7s\n(no prompt)", 0, False)
check("missing shell prompt -> fail", st == "fail" and "shell prompt" in why)

st, _ = br.classify_boot("Boot complete in 1.7s\n(no prompt)", 0, False,
                         require_shell_prompt=False)
check("ISO contract (no prompt required) without signals -> degraded", st == "degraded")

# exit_code=None isolates the serial-only path so this exercises the
# missing-PASS-marker branch, not the exit-code gate.
st, why = br.classify_boot("nothing useful here", None, False)
check("no PASS marker -> fail", st == "fail" and "PASS marker" in why)

# Missing a required milestone signal but otherwise clean -> degraded.
st, why = br.classify_boot("[PHASE0] BOOT_INFO\nBoot complete in 1s\nC:\\>", 0, False)
check("missing required signal -> degraded", st == "degraded" and "required signal" in why)

# ANSI color codes must not defeat marker matching.
st, _ = br.classify_boot("\x1b[32m[BOOT] ExitBootServices OK\x1b[0m\n"
                         "[PHASE0] BOOT_INFO\n\x1b[1mBoot complete in 1s\x1b[0m\nC:\\>",
                         0, False)
check("ANSI-wrapped markers still classify pass", st == "pass")

# exit_code is authoritative: a clean (even perfect) serial buffer with a
# nonzero launcher exit (preflight rc=2 leaving stale markers) is NOT a pass.
st, why = br.classify_boot(GOOD, 2, False)
check("clean serial + preflight exit 2 -> fail", st == "fail" and "exit 2" in why)
st, _ = br.classify_boot(GOOD, 1, False)
check("clean serial + launcher FAIL exit 1 -> fail", st == "fail")
# exit_code=None opts out of the exit check (raw-serial classification).
st, _ = br.classify_boot(GOOD, None, False)
check("exit_code=None classifies serial alone -> pass", st == "pass")

# ---- tier_flake_threshold ----
check("stable flake threshold is 0", br.tier_flake_threshold("stable") == 0)
check("nightly flake threshold > 0", br.tier_flake_threshold("nightly") >= 1)
try:
    br.tier_flake_threshold("bogus")
    check("unknown tier raises", False)
except ValueError:
    check("unknown tier raises", True)

# ---- aggregate_runs ----
def runs(*statuses):
    return [{"status": s} for s in statuses]

agg = br.aggregate_runs(runs("pass", "pass", "pass"), "stable")
check("stable all-pass -> pass", agg["decision"] == "pass" and agg["flake_count"] == 0)

agg = br.aggregate_runs(runs("pass", "fail", "pass"), "stable")
check("stable one fail -> fail (threshold 0)", agg["decision"] == "fail" and agg["flake_count"] == 1)

agg = br.aggregate_runs(runs("pass", "fail", "pass"), "nightly")
check("nightly one flake within budget -> degraded", agg["decision"] == "degraded")

agg = br.aggregate_runs(runs("fail", "fail"), "nightly")
check("nightly two flakes over budget -> fail", agg["decision"] == "fail")

agg = br.aggregate_runs(runs("degraded", "pass"), "stable")
check("stable degraded counts as flake -> fail", agg["decision"] == "fail" and agg["flake_count"] == 1)

agg = br.aggregate_runs(runs("skip", "skip"), "stable")
check("all-skip -> skip", agg["decision"] == "skip")

agg = br.aggregate_runs(runs("pass", "skip"), "stable")
check("skip not a flake but partial skip -> incomplete (skip)",
      agg["flake_count"] == 0 and agg["decision"] == "skip")

agg = br.aggregate_runs(runs("fail", "skip"), "stable")
check("fail outranks skip in aggregate", agg["decision"] == "fail")

# flake AND skip together is neither a clean pass nor allow-skippable: an
# incomplete run that also flaked is a hard fail on every tier, so a within-
# budget flake cannot mask the skips and the skips cannot mask the flake.
agg = br.aggregate_runs(runs("fail", "skip"), "nightly")
check("nightly fail+skip -> fail (incomplete + flaky)", agg["decision"] == "fail")
agg = br.aggregate_runs(runs("degraded", "skip"), "nightly")
check("nightly degraded+skip -> fail (incomplete + flaky)", agg["decision"] == "fail")
# Pure within-budget flake (no skip) stays degraded; pure skip stays skip.
agg = br.aggregate_runs(runs("pass", "degraded"), "nightly")
check("nightly degraded no-skip stays degraded", agg["decision"] == "degraded")

# ---- build_result conforms to result.schema.json (frozen S1 shape) ----
rschema = json.load(open(br.RESULT_SCHEMA, encoding="utf-8"))
rv = jsonschema.Draft202012Validator(rschema)
res = br.build_result("boot-reliability", "qemu-tcg", "stable", "b1", "ci",
                      "2026-06-19T00:00:00Z", "pass", ["serial-000-cold.log"])
check("build_result validates against result.schema.json", not list(rv.iter_errors(res)))

# ---- build_reliability conforms to reliability.schema.json ----
relschema = json.load(open(br.RELIABILITY_SCHEMA, encoding="utf-8"))
relv = jsonschema.Draft202012Validator(relschema)
its = [
    {"iteration": 0, "phase": "cold", "status": "pass", "logs": ["s0.log"]},
    {"iteration": 1, "phase": "warm", "status": "fail", "reason": "timeout",
     "logs": ["s1.log"]},
]
rel = br.build_reliability("boot-reliability", "qemu-tcg", "nightly", "b1", "ci",
                           "2026-06-19T00:00:00Z", 1, 1, 30, its)
check("build_reliability validates against reliability.schema.json",
      not list(relv.iter_errors(rel)))
check("aggregate flake_count surfaced in reliability obj", rel["flake_count"] == 1)
check("classifier_version stamped", rel["classifier_version"] == br.CLASSIFIER_VERSION)


def rel_raises(name, cold, warm, its):
    try:
        br.build_reliability("r", "qemu-tcg", "stable", "b", "m",
                             "2026-06-19T00:00:00Z", cold, warm, 30, its)
        check(name, False)
    except ValueError:
        check(name, True)


# Completeness invariants: an incomplete/aborted run must not serialize as a
# schema-valid pass/skip (test-coverage finding 2).
rel_raises("empty iterations + nonzero counts -> raises", 3, 2, [])
rel_raises("count mismatch (len != cold+warm) -> raises", 1, 1,
           [{"iteration": 0, "phase": "cold", "status": "pass"}])
rel_raises("phase split mismatch -> raises", 2, 0,
           [{"iteration": 0, "phase": "cold", "status": "pass"},
            {"iteration": 1, "phase": "warm", "status": "pass"}])
rel_raises("duplicate iteration ids -> raises", 2, 0,
           [{"iteration": 0, "phase": "cold", "status": "pass"},
            {"iteration": 0, "phase": "cold", "status": "pass"}])
rel_raises("non-sequential iteration ids -> raises", 2, 0,
           [{"iteration": 0, "phase": "cold", "status": "pass"},
            {"iteration": 5, "phase": "cold", "status": "pass"}])
rel_raises("bad iteration phase -> raises", 1, 0,
           [{"iteration": 0, "phase": "lukewarm", "status": "pass"}])
rel_raises("zero requested iterations -> raises (no vacuous pass)", 0, 0, [])

# ---- registry consistency: every boot-cert.yml VM platform_class is mapped ----
try:
    import yaml
    matrix = yaml.safe_load(open(os.path.join(HERE, "boot-cert.yml"), encoding="utf-8"))
    vm_classes = {"qemu-whpx", "qemu-tcg", "virtualbox", "hyperv"}
    declared = set(matrix.get("platform_classes", []))
    mapped = set(br.LAUNCHERS)
    check("every VM platform_class in matrix has a launcher registry entry",
          (vm_classes & declared) <= mapped)
except ImportError:
    check("registry consistency (pyyaml present)", True)  # soft-skip without yaml

# Blocked platforms must declare an owner and have no live launcher.
for pc, spec in br.LAUNCHERS.items():
    if spec["launcher"] is None:
        check(f"{pc} blocked entry names an owner", bool(spec["blocked"]))

# Every live launcher must be runnable: argv[0] is a shell (bash/sh) OR an
# executable file -- a registry entry pointing straight at a non-executable
# script would PermissionError before booting (the default qemu-tcg path).
import shutil as _sh  # noqa: E402
_REPO = os.path.dirname(os.path.dirname(HERE))
for pc, spec in br.LAUNCHERS.items():
    lc = spec["launcher"]
    if lc is None:
        continue
    head = lc[0]
    runnable = (os.path.basename(head) in ("bash", "sh")
                or os.access(os.path.join(_REPO, head), os.X_OK)
                or _sh.which(head) is not None)
    check(f"{pc} launcher is shell-prefixed or executable", runnable)

# Launcher source guard: the shell prompt literal must be a SINGLE backslash
# (a doubled-backslash 'C:\\>' never matches a real C:\> prompt -> every clean
# boot would FAIL). Also assert fixed-string matching, not case-glob.
_REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
_LAUNCHER = os.path.join(_REPO_ROOT, "scripts", "machines", "boot-test-qemu.sh")
_lsrc = open(_LAUNCHER, encoding="utf-8").read()
check("launcher prompt literal is single-backslash C:\\>", "PROMPT_MARKER='C:\\>'" in _lsrc)
check("launcher uses grep -qF fixed-string matching", "grep -qF" in _lsrc)

# ---- POST16 core gate (consistency finding) + manifest parsing ----
import tempfile as _tf2  # noqa: E402
_fd, _man = _tf2.mkstemp(suffix=".env", dir=HERE)
with os.fdopen(_fd, "w") as _fh:
    _fh.write("POST16_REQUIRED=(a b)\nPOST16_REQUIRED_CODES=(0xdf20 0xDF21)\n")
_mk = br.load_post16_markers(_man)
os.remove(_man)
check("load_post16_markers parses + uppercases codes",
      _mk == ("[BOOT] POST 0xDF20", "[BOOT] POST 0xDF21"))
check("load_post16_markers absent manifest -> empty",
      br.load_post16_markers("/no/such/manifest.env") == ())

_rs = br.REQUIRED_SIGNALS + ("[BOOT] POST 0xDF20",)
st, why = br.classify_boot(GOOD, 0, False, required_signals=_rs)
check("missing required POST16 marker -> degraded (matches smoke core gate)",
      st == "degraded" and "0xDF20" in why)
st, _ = br.classify_boot(GOOD + "\n[BOOT] POST 0xDF20", 0, False, required_signals=_rs)
check("present required POST16 marker -> pass", st == "pass")


# ---- orchestration via mock launchers (no QEMU) ----
# Exercises run_reliability end-to-end: cold/warm sequencing, the first-warm
# vars bootstrap, skip-as-failure, and the exit-code-only (VBox-style) path.
import tempfile  # noqa: E402
import stat  # noqa: E402
import argparse  # noqa: E402

_MOCK_DIR = tempfile.mkdtemp(prefix="boot-reliability-mock.")
# Orchestration mocks do not emit POST16 codes; isolate the POST16 gate (pure-
# tested above) so it does not degrade the orchestration fixtures.
br.load_post16_markers = lambda *a, **k: ()

# Faithful QEMU-style launcher: seeds --vars if missing (warm bootstrap),
# writes a clean boot log to --serial-out, exits 0.
_MOCK_QEMU = os.path.join(_MOCK_DIR, "mock-qemu.sh")
with open(_MOCK_QEMU, "w", encoding="utf-8") as fh:
    fh.write(
        '#!/usr/bin/env bash\nserial=""; vars=""\n'
        'while [ $# -gt 0 ]; do case "$1" in\n'
        '  --serial-out) serial="$2"; shift 2;;\n'
        '  --vars) vars="$2"; shift 2;;\n'
        '  --disk|--timeout|--screenshot) shift 2;;\n'
        '  *) shift;; esac; done\n'
        '[ -n "$vars" ] && [ ! -f "$vars" ] && : > "$vars"\n'
        '[ -n "$serial" ] && printf "%s\\n" "[BOOT] ExitBootServices OK" '
        '"[PHASE0] BOOT_INFO" "Boot complete in 1s" "C:\\\\>" > "$serial"\n'
        'exit 0\n')
os.chmod(_MOCK_QEMU, os.stat(_MOCK_QEMU).st_mode | stat.S_IEXEC)

_MOCK_SKIP = os.path.join(_MOCK_DIR, "mock-skip.sh")
with open(_MOCK_SKIP, "w", encoding="utf-8") as fh:
    fh.write("#!/usr/bin/env bash\nexit 3\n")
os.chmod(_MOCK_SKIP, os.stat(_MOCK_SKIP).st_mode | stat.S_IEXEC)

_MOCK_VBOX_FAIL = os.path.join(_MOCK_DIR, "mock-vbox-fail.sh")
with open(_MOCK_VBOX_FAIL, "w", encoding="utf-8") as fh:
    fh.write("#!/usr/bin/env bash\nexit 1\n")
os.chmod(_MOCK_VBOX_FAIL, os.stat(_MOCK_VBOX_FAIL).st_mode | stat.S_IEXEC)

# Stateful mock: first invocation boots clean (exit 0), every later one skips
# (exit 3) -- a partial-skip run. Counter persisted in a file beside the script.
_MOCK_COUNTER = os.path.join(_MOCK_DIR, "counter")
_MOCK_PASS_THEN_SKIP = os.path.join(_MOCK_DIR, "mock-pass-then-skip.sh")
with open(_MOCK_PASS_THEN_SKIP, "w", encoding="utf-8") as fh:
    fh.write(
        '#!/usr/bin/env bash\nserial=""\n'
        'while [ $# -gt 0 ]; do case "$1" in\n'
        '  --serial-out) serial="$2"; shift 2;;\n'
        '  --disk|--timeout|--vars|--screenshot) shift 2;;\n'
        '  *) shift;; esac; done\n'
        f'c="{_MOCK_COUNTER}"\nn=0; [ -f "$c" ] && n=$(cat "$c"); n=$((n+1)); echo "$n" > "$c"\n'
        'if [ "$n" -eq 1 ]; then\n'
        '  [ -n "$serial" ] && printf "%s\\n" "[BOOT] ExitBootServices OK" '
        '"[PHASE0] BOOT_INFO" "Boot complete in 1s" "C:\\\\>" > "$serial"\n'
        '  exit 0\nfi\nexit 3\n')
os.chmod(_MOCK_PASS_THEN_SKIP, os.stat(_MOCK_PASS_THEN_SKIP).st_mode | stat.S_IEXEC)


def mk_args(**kw):
    base = dict(platform="qemu-tcg", tier="stable", row_id="boot-reliability",
                cold=2, warm=2, timeout=5, disk="build/system-disk.img",
                test_suite="", boot_conf_patch=[], screenshot=False,
                no_shell_prompt=False, build_id="b", machine_id="m",
                ts="2026-06-19T00:00:00Z", out="", allow_skip=False,
                launcher_cmd=None)
    base.update(kw)
    return argparse.Namespace(**base)


# Cold+warm run via the faithful mock: no first-warm exit-2 flake, warm-vars
# bootstrapped+persisted, clean pass. boot_conf_patch=[] so no disk mutation.
od = os.path.join(_MOCK_DIR, "out-cw")
rc = br.run_reliability(mk_args(launcher_cmd=["bash", _MOCK_QEMU], out=od))
rel = json.load(open(os.path.join(od, "reliability.json"), encoding="utf-8"))
check("cold+warm mock run returns 0", rc == 0)
check("cold+warm mock decision pass (no first-warm flake)", rel["decision"] == "pass")
check("all 4 iterations recorded", len(rel["iterations"]) == 4)
check("warm-vars.fd bootstrapped + persisted", os.path.exists(os.path.join(od, "warm-vars.fd")))
check("no fail iterations in clean mock run", rel["fail_count"] == 0)

# Whole-run skip must be a release-gate FAILURE by default, success with --allow-skip.
rc = br.run_reliability(mk_args(launcher_cmd=["bash", _MOCK_SKIP], warm=0,
                                out=os.path.join(_MOCK_DIR, "out-skip")))
check("all-skip without --allow-skip -> nonzero", rc == 1)
rc = br.run_reliability(mk_args(launcher_cmd=["bash", _MOCK_SKIP], warm=0,
                                allow_skip=True, out=os.path.join(_MOCK_DIR, "out-skip2")))
check("all-skip with --allow-skip -> 0", rc == 0)

# Exit-code-only launcher (VBox platform, accepts=set()): no serial captured,
# verdict comes from the exit code. A mock exiting 1 must classify fail.
rc = br.run_reliability(mk_args(platform="virtualbox",
                                launcher_cmd=["bash", _MOCK_VBOX_FAIL],
                                cold=1, warm=0,
                                out=os.path.join(_MOCK_DIR, "out-vbox")))
relv2 = json.load(open(os.path.join(_MOCK_DIR, "out-vbox", "reliability.json"),
                      encoding="utf-8"))
check("exit-code-only launcher fail (exit 1) -> fail", relv2["iterations"][0]["status"] == "fail" and rc == 1)

# Partial skip (1 pass + 4 skip) on stable must NOT certify -- release-gate fail.
rc = br.run_reliability(mk_args(launcher_cmd=["bash", _MOCK_PASS_THEN_SKIP],
                                cold=5, warm=0,
                                out=os.path.join(_MOCK_DIR, "out-partial")))
relp = json.load(open(os.path.join(_MOCK_DIR, "out-partial", "reliability.json"),
                     encoding="utf-8"))
check("partial skip (1 pass + 4 skip) -> nonzero", rc == 1)
check("partial skip decision is skip (incomplete)", relp["decision"] == "skip")
check("partial skip records the 1 real pass", relp["pass_count"] == 1 and relp["skip_count"] == 4)

# --allow-skip must NOT excuse a partial skip (some iterations ran) -- only a
# whole-run skip where nothing booted. Reset the counter so iter 0 passes again.
os.remove(_MOCK_COUNTER)
rc = br.run_reliability(mk_args(launcher_cmd=["bash", _MOCK_PASS_THEN_SKIP],
                                cold=5, warm=0, allow_skip=True,
                                out=os.path.join(_MOCK_DIR, "out-partial-as")))
check("partial skip + --allow-skip still nonzero", rc == 1)

# Whole-run skip + --allow-skip -> 0 (already covered above with mock-skip).
# Disk guard: --test-suite/--boot-conf-patch on a non-default --disk is rejected
# at argparse time (SystemExit), never silently booting the wrong image.
try:
    br.main(["--platform", "qemu-tcg", "--tier", "stable", "--cold", "1",
             "--warm", "0", "--disk", "build/other.img", "--test-suite", "mm"])
    check("non-default --disk with --test-suite rejected", False)
except SystemExit as e:
    check("non-default --disk with --test-suite rejected", e.code != 0)

# Serial cap: a runaway log (> cap) is a bounded FAIL even with good markers,
# instead of OOMing the host. Shrink the cap and emit good markers + padding.
_orig_cap = br.SERIAL_READ_CAP
br.SERIAL_READ_CAP = 200
_mock_big = os.path.join(_MOCK_DIR, "mock-big.sh")
with open(_mock_big, "w", encoding="utf-8") as fh:
    fh.write('#!/usr/bin/env bash\nserial=""\n'
             'while [ $# -gt 0 ]; do case "$1" in\n'
             '  --serial-out) serial="$2"; shift 2;;\n'
             '  --disk|--timeout|--vars|--screenshot) shift 2;;\n'
             '  *) shift;; esac; done\n'
             '{ printf "%s\\n" "[BOOT] ExitBootServices OK" "[PHASE0] BOOT_INFO" '
             '"Boot complete in 1s" "C:\\\\>"; head -c 5000 /dev/zero | tr "\\0" "x"; } '
             '> "$serial"\nexit 0\n')
os.chmod(_mock_big, os.stat(_mock_big).st_mode | stat.S_IEXEC)
rc = br.run_reliability(mk_args(launcher_cmd=["bash", _mock_big], cold=1, warm=0,
                                out=os.path.join(_MOCK_DIR, "out-big")))
relb = json.load(open(os.path.join(_MOCK_DIR, "out-big", "reliability.json"),
                     encoding="utf-8"))
check("runaway serial (> cap) -> bounded fail",
      relb["iterations"][0]["status"] == "fail"
      and "runaway" in (relb["iterations"][0]["reason"] or ""))
br.SERIAL_READ_CAP = _orig_cap

# Non-repo-cwd: --disk is relative, but the flock + launched image must both
# resolve under REPO_ROOT, not the caller cwd (re-adversarial finding).
_lockf = os.path.join(_REPO_ROOT, "build", "system-disk.img.boot-cert.lock")
try:
    os.remove(_lockf)
except OSError:
    pass
_cwd0 = os.getcwd()
_tmpcwd = tempfile.mkdtemp(prefix="boot-reliability-cwd.")
try:
    os.chdir(_tmpcwd)
    br.run_reliability(mk_args(launcher_cmd=["bash", _MOCK_QEMU], cold=1, warm=0,
                               out=os.path.join(_MOCK_DIR, "out-cwd")))
finally:
    os.chdir(_cwd0)
check("relative --disk locks under REPO_ROOT regardless of cwd",
      os.path.exists(_lockf))
shutil_cwd = __import__("shutil"); shutil_cwd.rmtree(_tmpcwd, ignore_errors=True)

import shutil  # noqa: E402
shutil.rmtree(_MOCK_DIR, ignore_errors=True)

print(f"boot-reliability self-test: {PASS} passed, {FAIL} failed")
sys.exit(1 if FAIL else 0)
