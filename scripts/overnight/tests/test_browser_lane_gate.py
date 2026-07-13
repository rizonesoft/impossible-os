#!/usr/bin/env python3
# Protects I5 (2026-07-13 findings): the ChromeMCP browser lane is FAIL-SAFE OFF.
# The old gate acquired the lane unless OVERNIGHT_NO_CHROMEMCP was set, but that
# env drop-in failed to propagate through the systemd/watchdog chain, so all 8
# kernel runs claimed an unused lane. The new gate acquires ONLY on an explicit
# positive signal (env or sentinel file), so a propagation failure now fails safe.
import os
import subprocess
import sys
import tempfile
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
GATE = HERE.parent / "browser-lane-enabled.sh"


def _gate(env_overrides, sentinel_exists=False):
    with tempfile.TemporaryDirectory() as d:
        sentinel = pathlib.Path(d) / "overnight-with-browser"
        if sentinel_exists:
            sentinel.write_text("")
        env = dict(os.environ)
        env.pop("OVERNIGHT_WITH_BROWSER", None)
        env.pop("OVERNIGHT_NO_CHROMEMCP", None)
        env["OVERNIGHT_WITH_BROWSER_SENTINEL"] = str(sentinel)
        env.update(env_overrides)
        return subprocess.run(["bash", str(GATE)], env=env).returncode


def test_default_skips_lane():
    # No env, no sentinel -> skip (exit 1). This is the kernel-run common case.
    assert _gate({}) == 1


def test_env_signal_enables():
    assert _gate({"OVERNIGHT_WITH_BROWSER": "1"}) == 0


def test_sentinel_enables():
    assert _gate({}, sentinel_exists=True) == 0


def test_no_chromemcp_override_wins_over_env():
    # Hard override must skip even when a positive signal is present.
    assert _gate({"OVERNIGHT_WITH_BROWSER": "1", "OVERNIGHT_NO_CHROMEMCP": "1"}) == 1


def test_no_chromemcp_override_wins_over_sentinel():
    assert _gate({"OVERNIGHT_NO_CHROMEMCP": "1"}, sentinel_exists=True) == 1


def test_absent_sentinel_path_skips():
    # A sentinel path that does not exist must not enable the lane.
    assert _gate({"OVERNIGHT_WITH_BROWSER_SENTINEL": "/no/such/sentinel"}) == 1


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
    print("PASS: browser-lane-gate (I5)")
