#!/usr/bin/env python3
"""Boot-reliability gate for the boot certification matrix -- TODO-28 S2.

Drives a standardized VM launcher N cold + M warm reboot times on one
(row, platform_class, tier), classifies each iteration's captured serial with
the canonical boot-log contract (fail/absent patterns take precedence over PASS
markers; required POST16 + residual signals gate "degraded"), reduces the run
to a flake decision (stable tier tolerates 0 flakes), and emits:

  * one result.schema.json object per iteration (the frozen S1 single-run shape)
  * one reliability.schema.json aggregate (S2) the S9 release gate consumes.

The launcher registry standardizes QEMU WHPX/TCG, VirtualBox, and Hyper-V behind
the uniform --disk/--timeout/exit-code contract the per-format launchers already
use (scripts/release/boot-test-*.sh, scripts/machines/boot-test-qemu.sh). The
Windows-side WHPX/Hyper-V live runner is owned by TODO-06 (boot-test-whpx.ps1);
until it lands those classes register but cannot self-drive on a Linux host.

Pure functions (classify_boot / aggregate_runs / build_result / build_reliability)
carry the gate's verdict logic and are unit-tested with canned serial fixtures in
test_boot_reliability.py -- no live VM, mirroring the WSL no-QEMU constraint.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
RESULT_SCHEMA = os.path.join(HERE, "result.schema.json")
RELIABILITY_SCHEMA = os.path.join(HERE, "reliability.schema.json")

# Bump when the PASS/FAIL/required-signal semantics below change, so an old
# aggregate is never silently re-interpreted by a newer release gate.
CLASSIFIER_VERSION = 1

# scripts/patch-boot-conf.sh hardcodes this image, so --test-suite /
# --boot-conf-patch are only meaningful when the gate boots the same image.
DEFAULT_DISK = "build/system-disk.img"

# Canonical boot-log contract -- mirrors scripts/test-smoke.sh. PASS requires
# ALL of PASS_MARKERS (the shell prompt is gated by require_shell_prompt so an
# ISO / headless row contract can drop it). FAIL_MARKERS and ABSENT_MARKERS
# take precedence over any PASS marker that appeared earlier in the log.
PASS_MARKERS = ("Boot complete in",)
SHELL_PROMPT = "C:\\>"
FAIL_MARKERS = (
    "KERNEL PANIC", "ASSERT FAILED", "triple fault",
    "General Protection Fault", "Page Fault", "Double Fault",
    "ExitBootServices failed", "Kernel ELF corrupt", "BOOT HALT",
)
# Must NOT appear on a clean boot (degraded firmware/memory-map path).
ABSENT_MARKERS = (
    "mmap: overlap resolved", "mmap: carve at cap", "Memory map entry",
)
# Booted but missing one of these = degraded (real boot, not a clean cert).
REQUIRED_SIGNALS = (
    "[BOOT] ExitBootServices OK", "[PHASE0] BOOT_INFO",
)

_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]|\x1b[=>]")

# Per-tier flake budget: a flake is an iteration that ran but did not pass.
# stable tolerates ZERO (any non-pass iteration fails the gate); pre-release
# tiers tolerate a small number so a single transient does not red-light an
# overnight nightly. Skips never count as flakes.
TIER_FLAKE_THRESHOLD = {"nightly": 1, "rc": 1, "stable": 0}

# Launcher registry: platform_class -> how to drive one boot. `launcher` is an
# argv prefix invoked with the universal --disk/--timeout contract (None = no
# Linux-host self-driver yet). `accepts` is the subset of the OPTIONAL
# standardized flags the launcher understands ("serial-out", "vars",
# "screenshot"); _run_once appends only those, so a launcher that classifies
# internally (boot-test-vbox.sh, which captures serial via its own pipe and
# does NOT take --serial-out) is driven by its exit code alone instead of being
# handed an arg it rejects with usage rc=2. `caps` advertises which artifacts
# the launcher can capture. `cold_warm` = honors the --vars firmware NVRAM axis.
# `blocked` names the owner of a not-yet-available live runner.
LAUNCHERS = {
    "qemu-tcg": {
        # bash-prefixed so the launcher runs regardless of the script's file
        # mode (a git checkout may land it 100644 on some hosts).
        "launcher": ["bash", "scripts/machines/boot-test-qemu.sh", "--accel", "tcg"],
        "accepts": {"serial-out", "vars", "screenshot"},
        "caps": {"serial": True, "screenshot": True, "blackbox": True},
        "cold_warm": True,
        "blocked": None,
    },
    "qemu-whpx": {
        # Linux host cannot run WHPX; the Windows-side non-interactive runner
        # is owned by TODO-06 (scripts/machines/boot-test-whpx.ps1).
        "launcher": None,
        "accepts": {"serial-out", "vars", "screenshot"},
        "caps": {"serial": True, "screenshot": True, "blackbox": True},
        "cold_warm": True,
        "blocked": "TODO-06 boot-test-whpx.ps1",
    },
    "virtualbox": {
        # boot-test-vbox.sh self-classifies (exit 0 pass / 1 fail / 3 skip) and
        # captures serial through its own pipe -- it accepts only --disk/--timeout.
        "launcher": ["bash", "scripts/release/boot-test-vbox.sh"],
        "accepts": set(),
        "caps": {"serial": True, "screenshot": False, "blackbox": True},
        "cold_warm": False,  # VBox harness manages its own throwaway VM state
        "blocked": None,
    },
    "hyperv": {
        "launcher": None,
        "accepts": {"serial-out", "vars"},
        "caps": {"serial": True, "screenshot": False, "blackbox": True},
        "cold_warm": True,
        "blocked": "TODO-06 boot-test-whpx.ps1",
    },
}

# Exit-code -> status for launchers driven by exit code alone (no captured
# serial to classify). Mirrors the per-format launcher contract.
EXIT_STATUS = {0: ("pass", None), 3: ("skip", "launcher reported host SKIP"),
               2: ("fail", "launcher preflight error (exit 2)")}


def strip_ansi(text):
    return _ANSI_RE.sub("", text or "")


def classify_boot(serial_text, exit_code, timed_out,
                  require_shell_prompt=True,
                  pass_markers=PASS_MARKERS, shell_prompt=SHELL_PROMPT,
                  fail_markers=FAIL_MARKERS, absent_markers=ABSENT_MARKERS,
                  required_signals=REQUIRED_SIGNALS):
    """Classify one boot's captured serial -> (status, reason).

    Precedence (matches scripts/test-smoke.sh, fail-before-pass):
      1. any fail marker present                  -> fail
      2. any absent (forbidden) marker present    -> fail
      3. timed out                                -> fail
      4. nonzero launcher exit code               -> fail
      5. PASS markers (+ shell prompt if required) absent -> fail
      6. a required milestone signal missing      -> degraded
      7. otherwise                                -> pass
    `exit_code` is AUTHORITATIVE: the launcher contract returns 0 only when it
    observed the PASS markers in a fresh capture, so a nonzero exit (preflight
    error, crash, or the launcher's own FAIL verdict) can never be overridden
    by PASS markers still in the serial buffer -- a reused output dir with
    stale markers must not bless a failed attempt. `None` opts a caller out of
    the exit check (e.g. classifying a raw serial blob in isolation). The
    timeout-after-success case sets timed_out=True and is already a fail, so
    there is no legitimate "PASS log + nonzero exit" path.
    """
    txt = strip_ansi(serial_text)
    for p in fail_markers:
        if p in txt:
            return "fail", f"fail pattern: {p}"
    for p in absent_markers:
        if p in txt:
            return "fail", f"forbidden pattern: {p}"
    if timed_out:
        return "fail", "boot timed out"
    if exit_code is not None and exit_code != 0:
        return "fail", f"launcher exit {exit_code}"
    missing_pass = [p for p in pass_markers if p not in txt]
    if missing_pass:
        return "fail", f"missing PASS marker(s): {', '.join(missing_pass)}"
    if require_shell_prompt and shell_prompt not in txt:
        return "fail", f"missing shell prompt: {shell_prompt}"
    missing_sig = [s for s in required_signals if s not in txt]
    if missing_sig:
        return "degraded", f"missing required signal(s): {', '.join(missing_sig)}"
    return "pass", None


def tier_flake_threshold(tier):
    if tier not in TIER_FLAKE_THRESHOLD:
        raise ValueError(f"unknown tier: {tier}")
    return TIER_FLAKE_THRESHOLD[tier]


def aggregate_runs(iterations, tier):
    """Reduce per-iteration statuses to a release decision.

    `iterations` is a list of dicts each with at least a "status" key. A flake
    is any iteration that RAN but did not pass (fail/degraded); skips never
    count. The decision is fail when flake_count exceeds the tier budget, skip
    when every iteration skipped, degraded when within budget but not clean,
    else pass.
    """
    threshold = tier_flake_threshold(tier)
    counts = {"pass": 0, "fail": 0, "degraded": 0, "skip": 0}
    for it in iterations:
        st = it.get("status")
        if st not in counts:
            raise ValueError(f"bad iteration status: {st!r}")
        counts[st] += 1
    flake_count = counts["fail"] + counts["degraded"]
    # A clean certification needs BOTH zero flakes (beyond the tier budget) AND
    # zero skips (every requested iteration booted). flakes and skips are
    # orthogonal failure modes, so decision precedence handles every combination
    # without either masking the other:
    #   fail     -- flakes exceed the tier budget, OR the run is BOTH incomplete
    #               (>=1 skip) AND flaky (>=1 fail/degraded). The second clause
    #               is never excusable by --allow-skip (a real boot flaked) and
    #               is never softened to a within-budget "degraded" (a boot also
    #               failed to run).
    #   skip     -- incomplete (>=1 skip) but otherwise clean: the N cold + M
    #               warm invariant did not hold; --allow-skip can excuse this.
    #   degraded -- complete run, flakes within the tier budget.
    #   pass     -- every requested iteration booted clean.
    if flake_count > threshold:
        decision = "fail"
        reason = f"{flake_count} flake(s) > tier budget {threshold}"
    elif counts["skip"] > 0 and flake_count > 0:
        decision = "fail"
        reason = (f"incomplete run: {counts['skip']} skipped with "
                  f"{flake_count} flake(s)")
    elif counts["skip"] > 0:
        decision = "skip"
        reason = f"{counts['skip']} iteration(s) skipped (incomplete run)"
    elif flake_count > 0:
        decision = "degraded"
        reason = f"{flake_count} flake(s) within tier budget {threshold}"
    else:
        decision, reason = "pass", None
    return {
        "pass_count": counts["pass"], "fail_count": counts["fail"],
        "degraded_count": counts["degraded"], "skip_count": counts["skip"],
        "flake_count": flake_count, "flake_threshold": threshold,
        "decision": decision, "reason": reason,
    }


def build_result(row_id, platform_class, tier, build_id, machine_id, ts,
                 status, logs=None, reason=None, artifact_id=None):
    """One result.schema.json (S1) object for a single iteration/run."""
    obj = {
        "row_id": row_id, "status": status, "build_id": build_id,
        "artifact_id": artifact_id, "machine_id": machine_id, "tier": tier,
        "platform_class": platform_class, "ts": ts,
        "logs": list(logs or []), "reason": reason,
    }
    return obj


def build_reliability(row_id, platform_class, tier, build_id, machine_id, ts,
                      cold_count, warm_count, timeout_sec, iterations):
    """One reliability.schema.json (S2) aggregate from classified iterations.

    `iterations` is a list of dicts {iteration, phase, status, reason?, logs?}.

    Enforces completeness invariants so an incomplete or aborted run cannot be
    serialized as a schema-valid pass/skip the S9 release gate would trust:
      * exactly cold_count + warm_count iterations were executed,
      * the cold/warm phase split matches the requested counts,
      * iteration ids are unique and sequential 0..N-1.
    A violation is a hard ValueError (run bug), not a silent skip.
    """
    expected = cold_count + warm_count
    if expected == 0:
        # A zero-iteration aggregate would reduce to decision=pass (no flakes,
        # no skips) and certify nothing -- enforce the invariant in the core
        # builder so a direct caller can't bypass the argparse guard.
        raise ValueError("no iterations requested (cold_count + warm_count == 0)")
    if len(iterations) != expected:
        raise ValueError(
            f"incomplete run: {len(iterations)} iterations but "
            f"cold_count+warm_count={expected}")
    phase_counts = {"cold": 0, "warm": 0}
    ids = []
    for it in iterations:
        ph = it.get("phase")
        if ph not in phase_counts:
            raise ValueError(f"bad iteration phase: {ph!r}")
        phase_counts[ph] += 1
        ids.append(it.get("iteration"))
    if phase_counts["cold"] != cold_count or phase_counts["warm"] != warm_count:
        raise ValueError(
            f"phase split {phase_counts} != requested cold={cold_count} "
            f"warm={warm_count}")
    if ids != list(range(expected)):
        raise ValueError(f"iteration ids not unique/sequential 0..{expected-1}: {ids}")
    agg = aggregate_runs(iterations, tier)
    obj = {
        "row_id": row_id, "platform_class": platform_class, "tier": tier,
        "build_id": build_id, "machine_id": machine_id, "ts": ts,
        "cold_count": cold_count, "warm_count": warm_count,
        "timeout_sec": timeout_sec,
        "classifier_version": CLASSIFIER_VERSION,
        "iterations": [
            {
                "iteration": it["iteration"], "phase": it["phase"],
                "status": it["status"], "reason": it.get("reason"),
                "logs": list(it.get("logs") or []),
            }
            for it in iterations
        ],
    }
    obj.update(agg)
    return obj


# --------------------------------------------------------------------------
# Orchestration (live -- validated by running it; not exercised by the unit
# test, which has no QEMU). Kept thin: pure verdict logic lives above.
# --------------------------------------------------------------------------

def _apply_boot_conf(patches, test_suite):
    """Patch boot.conf in the disk image via the existing host tool, then a
    matching reset is the caller's responsibility on the cold-series boundary.
    Returns the key/value argv applied (for the reset symmetry)."""
    kv = []
    for p in patches or []:
        if "=" not in p:
            raise ValueError(f"--boot-conf-patch needs key=value, got {p!r}")
        k, v = p.split("=", 1)
        kv += [k, v]
    if test_suite:
        kv += ["test", "1", "test_suite", test_suite]
    if kv:
        subprocess.run(["bash", "scripts/patch-boot-conf.sh", *kv],
                       cwd=REPO_ROOT, check=True)
    return kv


def _reset_boot_conf():
    subprocess.run(["bash", "scripts/patch-boot-conf.sh", "reset"],
                   cwd=REPO_ROOT, check=False)


def _run_once(launcher_argv, accepts, disk, timeout_sec, serial_out, vars_path,
              screenshot):
    """Invoke one launcher iteration honoring the launcher's accepted flags.

    Returns (exit_code, timed_out, serial, captured) where `captured` is True
    only when this launcher takes --serial-out (so the caller knows whether to
    re-classify the serial or trust the launcher's exit code).
    """
    argv = list(launcher_argv) + ["--disk", disk, "--timeout", str(timeout_sec)]
    captured = "serial-out" in accepts
    if captured:
        argv += ["--serial-out", serial_out]
        # Truncate before launch: a launcher that exits on preflight (rc 2)
        # before it would truncate --serial-out must not leave a previous
        # iteration's PASS markers for classify_boot to misread as a clean boot.
        try:
            open(serial_out, "w", encoding="utf-8").close()
        except OSError:
            pass
    if vars_path is not None and "vars" in accepts:
        argv += ["--vars", vars_path]
    if screenshot and "screenshot" in accepts:
        argv += ["--screenshot", screenshot]
    timed_out = False
    # Hard wall a little past the launcher's own timeout so a wedged launcher
    # cannot hang the whole reliability series. Run the launcher in its OWN
    # process group (start_new_session) so a timeout kills the shell AND the
    # QEMU/VBox child it spawned -- otherwise an orphan VM survives Python's
    # single-process kill and races the next iteration on the shared disk /
    # warm vars / serial artifacts.
    import signal

    def _killpg(sig):
        try:
            os.killpg(os.getpgid(proc.pid), sig)
        except (ProcessLookupError, PermissionError):
            pass

    # Block SIGINT across the spawn so a Ctrl-C that lands in the sub-bytecode
    # window between Popen returning the child and `proc` being bound (and the
    # kill-protected try becoming active) is held PENDING rather than orphaning
    # the new-session launcher. The mask is restored as the first statement
    # inside the kill-protected wait region, so the deferred KeyboardInterrupt is
    # delivered only once _killpg can reap the group. ONLY SIGINT is masked --
    # it raises a catchable KeyboardInterrupt; SIGTERM's default disposition is
    # immediate termination (not a catchable exception), so masking-then-
    # unmasking it would kill the parent before cleanup, defeating the purpose.
    # A SIGTERM during the brief spawn window leaves at most one orphan launcher
    # the operator reaps -- an acceptable residual for a host CI tool.
    _blk = {signal.SIGINT}
    _prev_mask = signal.pthread_sigmask(signal.SIG_BLOCK, _blk)
    try:
        proc = subprocess.Popen(argv, cwd=REPO_ROOT, start_new_session=True)
    except BaseException:
        signal.pthread_sigmask(signal.SIG_SETMASK, _prev_mask)
        raise
    try:
        signal.pthread_sigmask(signal.SIG_SETMASK, _prev_mask)
        proc.wait(timeout=timeout_sec + 30)
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        rc, timed_out = 124, True
        _killpg(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            _killpg(signal.SIGKILL)
            proc.wait()
    except BaseException:
        # KeyboardInterrupt / any non-timeout interruption: do NOT leave the
        # launcher's whole process group (QEMU/VBox child included) running
        # against the disk while the caller's finally resets boot.conf. Kill
        # the group and reap it before propagating.
        _killpg(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            _killpg(signal.SIGKILL)
            proc.wait()
        raise
    serial = ""
    if captured:
        try:
            with open(serial_out, encoding="utf-8", errors="replace") as fh:
                serial = fh.read()
        except OSError:
            pass
    return rc, timed_out, serial, captured


def _atomic_write_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(obj, fh, indent=2)
    os.replace(tmp, path)


def run_reliability(args):
    spec = LAUNCHERS.get(args.platform)
    if spec is None:
        sys.stderr.write(f"boot-reliability: unknown platform {args.platform!r}\n")
        return 2
    # --launcher-cmd overrides the registry launcher (live dry-run / testing);
    # it inherits the platform's accepts/caps/cold_warm contract.
    launcher = args.launcher_cmd if args.launcher_cmd else spec["launcher"]
    if launcher is None:
        sys.stderr.write(
            f"boot-reliability: platform {args.platform!r} has no Linux-host "
            f"launcher (blocked: {spec['blocked']}); cannot self-drive here\n")
        return 3
    accepts = spec["accepts"]

    # Default to a per-run (pid) namespace so two concurrent reliability runs do
    # not truncate/read the same serial logs, race on warm-vars.fd, or overwrite
    # each other's aggregate. An explicit --out is taken verbatim (caller owns
    # uniqueness, e.g. a CI matrix that already shards by platform/tier).
    out_dir = args.out if args.out else os.path.join(
        REPO_ROOT, "build", "boot-cert", "reliability", f"run-{os.getpid()}")
    # Absolute so the parent (which reads serial logs) and the launcher child
    # (run with cwd=REPO_ROOT) resolve the same paths regardless of caller cwd.
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)
    require_prompt = not args.no_shell_prompt
    iterations = []
    idx = 0

    # Warm series shares one OVMF_VARS file so reboot N inherits reboot N-1's
    # firmware NVRAM; the launcher seeds it fresh from the template on the first
    # warm iteration (missing path) and reuses it after. Cold runs get a fresh
    # ephemeral copy inside the launcher.
    warm_vars = os.path.join(out_dir, "warm-vars.fd") if spec["cold_warm"] else None
    if warm_vars and os.path.exists(warm_vars):
        os.remove(warm_vars)

    patched = bool(args.boot_conf_patch or args.test_suite)
    phases = [("cold", args.cold)] + ([("warm", args.warm)] if args.warm else [])
    try:
        for phase, count in phases:
            if phase == "cold":
                _reset_boot_conf()
                _apply_boot_conf(args.boot_conf_patch, args.test_suite)
            for _ in range(count):
                serial_out = os.path.join(out_dir, f"serial-{idx:03d}-{phase}.log")
                shot = (os.path.join(out_dir, f"screen-{idx:03d}-{phase}.ppm")
                        if args.screenshot and spec["caps"]["screenshot"] else None)
                vars_path = warm_vars if (spec["cold_warm"] and phase == "warm") else None
                rc, timed_out, serial, captured = _run_once(
                    launcher, accepts, args.disk, args.timeout, serial_out,
                    vars_path, shot)
                if captured:
                    # rc==3 means host SKIP even for a serial-capturing launcher.
                    if rc == 3:
                        status, reason = "skip", "launcher reported host SKIP"
                    else:
                        status, reason = classify_boot(
                            serial, rc, timed_out,
                            require_shell_prompt=require_prompt)
                else:
                    # Exit-code-only launcher (e.g. VBox): trust its verdict.
                    if timed_out:
                        status, reason = "fail", "boot timed out"
                    else:
                        status, reason = EXIT_STATUS.get(
                            rc, ("fail", f"launcher exit {rc}"))
                logs = [serial_out] + ([shot] if shot else []) if captured else \
                    ([shot] if shot else [])
                iterations.append({"iteration": idx, "phase": phase,
                                   "status": status, "reason": reason, "logs": logs})
                idx += 1
    finally:
        # Always restore boot.conf even if a launcher raised mid-series, so a
        # crashed run can never leave the certified image patched for the next.
        if patched:
            _reset_boot_conf()

    rel = build_reliability(
        args.row_id, args.platform, args.tier, args.build_id, args.machine_id,
        args.ts, args.cold, args.warm, args.timeout, iterations)

    # Per-iteration single-run results (frozen S1 shape) alongside the aggregate.
    results = [
        build_result(args.row_id, args.platform, args.tier, args.build_id,
                     args.machine_id, args.ts, it["status"], it["logs"],
                     it["reason"])
        for it in iterations
    ]
    _validate(rel, results)

    rel_path = os.path.join(out_dir, "reliability.json")
    _atomic_write_json(rel_path, rel)
    _atomic_write_json(os.path.join(out_dir, "results.json"), results)

    sys.stderr.write(
        f"boot-reliability {args.platform}/{args.tier}: {rel['decision']} "
        f"(flakes {rel['flake_count']}/{rel['flake_threshold']}, "
        f"pass {rel['pass_count']} fail {rel['fail_count']} "
        f"degraded {rel['degraded_count']} skip {rel['skip_count']}) -> {rel_path}\n")
    # Exit-code contract: pass/degraded met the tier gate -> 0; fail -> 1; a
    # whole-run skip (nothing actually booted -- e.g. host lacks the VM) is a
    # HARD FAIL for a release gate unless the caller opted in with --allow-skip,
    # so CI cannot ship without ever exercising the boot-reliability gate.
    if rel["decision"] in ("pass", "degraded"):
        return 0
    if rel["decision"] == "skip":
        # --allow-skip excuses ONLY a whole-run skip (host genuinely can't run
        # this platform -- nothing booted). A PARTIAL skip (some iterations ran)
        # is an incomplete N cold + M warm run and stays a failure regardless,
        # so allow-skip cannot bless a half-exercised reliability gate.
        whole_run_skip = (rel["pass_count"] == 0 and rel["fail_count"] == 0
                          and rel["degraded_count"] == 0)
        return 0 if (args.allow_skip and whole_run_skip) else 1
    return 1


def _validate(rel, results):
    try:
        import jsonschema
    except ImportError:
        sys.stderr.write("boot-reliability: jsonschema unavailable; skipping "
                         "schema validation\n")
        return
    with open(RELIABILITY_SCHEMA, encoding="utf-8") as fh:
        rs = json.load(fh)
    with open(RESULT_SCHEMA, encoding="utf-8") as fh:
        ss = json.load(fh)
    jsonschema.Draft202012Validator(rs).validate(rel)
    rv = jsonschema.Draft202012Validator(ss)
    for r in results:
        rv.validate(r)


def main(argv=None):
    ap = argparse.ArgumentParser(description="boot-reliability gate (TODO-28 S2)")
    ap.add_argument("--platform", required=True, choices=sorted(LAUNCHERS))
    ap.add_argument("--tier", required=True, choices=sorted(TIER_FLAKE_THRESHOLD))
    ap.add_argument("--row-id", default="boot-reliability")
    ap.add_argument("--cold", type=int, default=3)
    ap.add_argument("--warm", type=int, default=2)
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--disk", default=DEFAULT_DISK)
    ap.add_argument("--test-suite", default="")
    ap.add_argument("--boot-conf-patch", action="append", default=[],
                    metavar="KEY=VALUE")
    ap.add_argument("--screenshot", action="store_true")
    ap.add_argument("--no-shell-prompt", action="store_true",
                    help="row contract does not require the C:\\> prompt (ISO)")
    ap.add_argument("--build-id", default="dev")
    ap.add_argument("--machine-id", default="localhost")
    ap.add_argument("--ts", default="1970-01-01T00:00:00Z",
                    help="ISO-8601 UTC; pass the real stamp from the caller")
    ap.add_argument("--out", default="",
                    help="output dir (default: a per-pid run dir under "
                         "build/boot-cert/reliability/)")
    ap.add_argument("--allow-skip", action="store_true",
                    help="treat a whole-run skip (nothing booted) as success "
                         "(exit 0) instead of a release-gate failure")
    ap.add_argument("--launcher-cmd", nargs="+", default=None,
                    help="override the registry launcher argv (live dry-run / "
                         "testing); inherits the platform's accepts/caps")
    ap.add_argument("--self-check", action="store_true",
                    help="validate registry + schemas load, then exit")
    args = ap.parse_args(argv)
    if args.cold < 0 or args.warm < 0:
        ap.error("--cold/--warm must be >= 0")
    if args.cold + args.warm == 0:
        ap.error("at least one of --cold/--warm must be > 0")
    if (args.test_suite or args.boot_conf_patch) and args.disk != DEFAULT_DISK:
        # patch-boot-conf.sh only mutates DEFAULT_DISK; patching it while booting
        # a different --disk would certify an UNpatched artifact. Refuse rather
        # than silently boot the wrong image.
        ap.error(f"--test-suite/--boot-conf-patch require --disk {DEFAULT_DISK} "
                 f"(patch-boot-conf.sh only patches that image)")
    if args.self_check:
        _validate(build_reliability(args.row_id, args.platform, args.tier,
                                    "b", "m", args.ts, 1, 0, 1,
                                    [{"iteration": 0, "phase": "cold",
                                      "status": "pass"}]), [])
        print("boot-reliability self-check OK")
        return 0
    return run_reliability(args)


if __name__ == "__main__":
    sys.exit(main())
