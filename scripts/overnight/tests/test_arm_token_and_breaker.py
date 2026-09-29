#!/usr/bin/env python3
"""Arm-time token probe and breaker reset (2026-09-28 canary findings).

token-probe.sh: arming checked only that the token FILE existed, so a revoked
token armed cleanly and the run died at launch with a 401. The probe makes one
authenticated call and refuses (rc 3) on an auth failure. Refusal-direction
controls: a rejected token must refuse, and the token value must never appear
in the probe's output, even when the CLI echoes it back.

reset-breaker.sh: a fresh arm inherited the previous run's backoff and no-ship
streak and stopped at once. Refusal-direction control: it must NOT reset while a
run is live, and the launcher (watchdog relaunches) must never call it.
"""
from __future__ import annotations

import os
import pathlib
import stat
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
PROBE = REPO / "scripts/overnight/token-probe.sh"
RESET = REPO / "scripts/overnight/reset-breaker.sh"
TOKEN = "sk-ant-oat01-TESTTOKENVALUE-0123456789abcdefAA"


def _fake_claude(td, body):
    p = pathlib.Path(td) / "fake-claude"
    p.write_text("#!/bin/bash\n" + body + "\n")
    p.chmod(p.stat().st_mode | stat.S_IEXEC)
    return str(p)


def _probe(td, body, token_line=f"CLAUDE_CODE_OAUTH_TOKEN={TOKEN}"):
    env_file = pathlib.Path(td) / "token.env"
    if token_line is not None:
        env_file.write_text("# minted for tests\n" + token_line + "\n")
    env = dict(os.environ, CLAUDE_BIN=_fake_claude(td, body))
    r = subprocess.run(["bash", str(PROBE), str(env_file)], env=env,
                       capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout + r.stderr


def test_probe_passes_a_working_token_and_hands_it_over_by_env():
    with tempfile.TemporaryDirectory() as td:
        seen = pathlib.Path(td) / "seen"
        rc, out = _probe(td, f'printf "%s" "$CLAUDE_CODE_OAUTH_TOKEN" > {seen}; echo ok')
        assert rc == 0 and "authenticates" in out, (rc, out)
        assert seen.read_text() == TOKEN, "the probe must pass the token through the environment"
        assert TOKEN not in out


def test_probe_refuses_a_rejected_token():
    with tempfile.TemporaryDirectory() as td:
        rc, out = _probe(td, 'echo "Failed to authenticate. API Error: 401 OAuth access token is invalid." >&2; exit 1')
        assert rc == 3 and "REFUSED" in out, (rc, out)
        assert "claude setup-token" in out, out


def test_probe_never_prints_the_token_even_when_the_cli_echoes_it():
    with tempfile.TemporaryDirectory() as td:
        rc, out = _probe(td, 'echo "401 invalid token: $CLAUDE_CODE_OAUTH_TOKEN" >&2; exit 1')
        assert rc == 3, (rc, out)
        assert TOKEN not in out, "token value leaked into probe output"
        assert "[redacted]" in out, out


def test_probe_accepts_quoted_and_exported_token_lines():
    with tempfile.TemporaryDirectory() as td:
        seen = pathlib.Path(td) / "seen"
        rc, out = _probe(td, f'printf "%s" "$CLAUDE_CODE_OAUTH_TOKEN" > {seen}; echo ok',
                         token_line=f'export CLAUDE_CODE_OAUTH_TOKEN="{TOKEN}"')
        assert rc == 0 and seen.read_text() == TOKEN, (rc, out, seen.read_text())


def test_probe_is_inconclusive_not_refusing_on_other_failures():
    with tempfile.TemporaryDirectory() as td:
        rc, out = _probe(td, 'echo "Connection error: getaddrinfo ENOTFOUND" >&2; exit 1')
        assert rc == 0 and "inconclusive" in out, (rc, out)


def test_probe_skips_when_there_is_no_token_file():
    with tempfile.TemporaryDirectory() as td:
        rc, out = _probe(td, "exit 1", token_line=None)
        assert rc == 0 and "skipped" in out, (rc, out)


def _runtime(td):
    root = pathlib.Path(td)
    ov = root / ".claude/overnight"
    (ov / "state").mkdir(parents=True)
    (ov / "watchdog-backoff-until").write_text("1790000000 3\n")
    (ov / "state/unproductive-streak").write_text("3\n")
    (ov / "noship-streak").write_text("2\n")
    (ov / "NEEDS-OPERATOR.md").write_text("# Circuit breaker tripped\n")
    (ov / "keep-me").write_text("unrelated\n")
    return root, ov


def test_reset_clears_the_four_breaker_files_and_names_them():
    with tempfile.TemporaryDirectory() as td:
        root, ov = _runtime(td)
        env = dict(os.environ, RESET_BREAKER_ASSUME_STOPPED="1")
        r = subprocess.run(["bash", str(RESET), str(root), "overnight-test-unit"], env=env,
                           capture_output=True, text=True, timeout=30)
        assert r.returncode == 0, r.stderr
        for name in ("watchdog-backoff-until", "state/unproductive-streak", "noship-streak", "NEEDS-OPERATOR.md"):
            assert not (ov / name).exists(), f"{name} survived the reset"
        assert (ov / "keep-me").exists(), "reset must remove only breaker state"
        assert "noship-streak=2" in r.stdout and "unproductive-streak=3" in r.stdout, r.stdout


def test_reset_is_skipped_while_a_run_is_live():
    with tempfile.TemporaryDirectory() as td:
        root, ov = _runtime(td)
        bindir = pathlib.Path(td) / "bin"
        bindir.mkdir()
        fake = bindir / "systemctl"
        fake.write_text('#!/bin/bash\n# every timer reports active\nexit 0\n')
        fake.chmod(0o755)
        env = dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}")
        env.pop("RESET_BREAKER_ASSUME_STOPPED", None)
        r = subprocess.run(["bash", str(RESET), str(root), "overnight-test-unit"], env=env,
                           capture_output=True, text=True, timeout=30)
        assert "skipped" in r.stdout, r.stdout
        assert (ov / "noship-streak").exists() and (ov / "watchdog-backoff-until").exists(), \
            "a live run's breaker must survive a re-arm"


def test_only_the_arm_path_resets_the_breaker():
    launcher = (REPO / "scripts/overnight/overnight-launch.sh").read_text(encoding="utf-8")
    assert "reset-breaker.sh" not in launcher, "watchdog relaunches must honour the breaker"
    arm = (REPO / ".claude/skills/overnight-sequencer/arm-sequencer.sh").read_text(encoding="utf-8")
    assert "reset-breaker.sh" in arm and "token-probe.sh" in arm, "arm-sequencer must call both"
    # both run BEFORE the pre-arm health gate: its launcher DRYRUN honours a stale backoff
    gate = arm.index('scripts/overnight/pre-arm-check.sh" "$REPO_ROOT"')
    assert arm.index("reset-breaker.sh") < gate, "reset must precede the pre-arm DRYRUN"


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print("PASS: arm token probe + breaker reset")
