#!/usr/bin/env python3
# Protects the canary-tiering (P0.0, 2026-07-12): control-plane-match.sh's
# --flow-critical mode must classify deterministically-covered files (the
# control-plane-deterministic.txt allowlist) as NOT re-arming the watched canary,
# while every other control-plane path stays flow-critical. A regression here
# either re-introduces the "every runner change needs a canary" bottleneck (a
# det file wrongly flagged flow-critical) OR -- worse -- lets a flow-critical
# change skip the canary (a flow file wrongly excluded).
import subprocess, pathlib

HERE = pathlib.Path(__file__).resolve().parent
MATCH = HERE.parent / "control-plane-match.sh"

# Stable representatives (must stay classified as-is; both are real repo paths).
DET = "scripts/overnight/metrics-report.py"        # in the deterministic allowlist
DET2 = "scripts/overnight/review-envelope.py"      # allowlist
DET_DIR = "scripts/overnight/tests/anything.py"    # covered by the tests/ dir entry
FLOW = "scripts/overnight/overnight-launch.sh"     # flow-critical (lifecycle)
FLOW_HOOK = ".claude/hooks/run_phase_guard.py"     # flow-critical (phase machine)
FLOW_SPLIT = "scripts/overnight/section-manifest.py"  # flow-critical (drives SPLIT)
UNRELATED = "src/kernel/x.c"                        # not control-plane at all


def run(args):
    r = subprocess.run(["bash", str(MATCH)] + args, text=True, capture_output=True)
    return r.returncode, [ln for ln in r.stdout.splitlines() if ln.strip()]


def test_default_mode_unchanged():
    # Default (no flag) still matches BOTH det and flow control-plane files.
    rc, out = run([DET]);  assert rc == 0 and DET in out, (rc, out)
    rc, out = run([FLOW]); assert rc == 0 and FLOW in out, (rc, out)
    rc, out = run([UNRELATED]); assert rc == 1 and out == [], (rc, out)


def test_flowcritical_excludes_deterministic():
    for p in (DET, DET2, DET_DIR):
        rc, out = run(["--flow-critical", p])
        assert rc == 1 and out == [], ("must NOT be flow-critical", p, rc, out)


def test_flowcritical_includes_flow_paths():
    for p in (FLOW, FLOW_HOOK, FLOW_SPLIT):
        rc, out = run(["--flow-critical", p])
        assert rc == 0 and p in out, ("must be flow-critical", p, rc, out)


def test_flowcritical_mixed_returns_only_flow():
    rc, out = run(["--flow-critical", DET, FLOW, DET2, FLOW_HOOK])
    assert rc == 0 and out == [FLOW, FLOW_HOOK], (rc, out)


def test_flowcritical_non_control_plane_stays_empty():
    rc, out = run(["--flow-critical", UNRELATED])
    assert rc == 1 and out == [], (rc, out)


if __name__ == "__main__":
    test_default_mode_unchanged()
    test_flowcritical_excludes_deterministic()
    test_flowcritical_includes_flow_paths()
    test_flowcritical_mixed_returns_only_flow()
    test_flowcritical_non_control_plane_stays_empty()
    print("PASS: control-plane-tiering")
