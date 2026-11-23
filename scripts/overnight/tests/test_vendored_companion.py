#!/usr/bin/env python3
"""The vendored Codex companion every unattended review runs (tools/vendor/codex-companion/), under this gated
suite: its own tests, and the dispatcher driving the REAL companion against upstream's fake app-server."""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

import pytest

REPO = pathlib.Path(__file__).resolve().parents[3]
VENDOR = REPO / "tools/vendor/codex-companion"
TODO = "todo/00-infrastructure/TODO-01-developer-tooling-stack.md"

pytestmark = pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")


def _run(argv, **kw):
    """In its own session, and the session stopped afterwards: nothing this starts outlives the test."""
    proc = subprocess.Popen(argv, start_new_session=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, **kw)
    try:
        out, err = proc.communicate(timeout=240)
    finally:
        try:
            os.killpg(proc.pid, 9)
        except ProcessLookupError:
            pass
    return subprocess.CompletedProcess(argv, proc.returncode, out, err)


def test_the_companion_effort_tests_pass():
    done = _run(["node", "--test", str(VENDOR / "tests/effort.test.mjs")], cwd=str(REPO))
    assert done.returncode == 0, done.stdout[-2000:] + done.stderr[-2000:]


def _marked(prefix):
    found = []
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            env = pathlib.Path(f"/proc/{name}/environ").read_bytes().split(b"\0")
            state = pathlib.Path(f"/proc/{name}/stat").read_text().rsplit(") ", 1)[1][0]
        except (OSError, IndexError):
            continue
        if state != "Z" and any(e.startswith(f"CLAUDE_PLUGIN_DATA={prefix}".encode()) for e in env):
            found.append(int(name))
    return found


def test_a_real_pass_applies_its_effort_and_leaves_nothing_behind(tmp_path):
    """The dispatcher's default companion against the upstream fake app-server: the pin's effort reaches turn/start,
    and with TMPDIR inside the checkout the pass leaves no file there and no process of its plugin data running
    (round review, 2026-11-23: the broker's cxc-* directory and log survived and changed the candidate)."""
    repo = tmp_path / "repo"
    repo.mkdir()

    def git(*args):
        subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                        "-c", "core.hooksPath=/dev/null", *args], check=True, capture_output=True, timeout=60)
    git("init", "-q")
    (repo / "src/kernel").mkdir(parents=True)
    (repo / "src/kernel/a.c").write_text("int a;\n")
    git("add", "-A")
    git("commit", "-q", "-m", "a")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    installed = _run(["node", "--input-type=module", "-e",
                      f"import({json.dumps(str(VENDOR / 'tests/fake-codex-fixture.mjs'))})"
                      f".then(m => m.installFakeCodex({json.dumps(str(bindir))}))"])
    assert installed.returncode == 0, installed.stderr
    # The generated fake (a temp file of this test, not the vendored fixture) answers with the verdict lines a
    # plan pass owes, so the pass can earn credit and this test sees a regression that stops it doing so.
    fake = bindir / "codex"
    text = fake.read_text()
    assert 'next_steps: ["Add an empty-state test."]' in text
    fake.write_text(text.replace('next_steps: ["Add an empty-state test."]',
                                 'next_steps: ["LENS plan: clean", "HIGHEST: none"]'))
    inside = repo / "scratch"
    inside.mkdir()
    reviews = tmp_path / "reviews"
    env = {k: v for k, v in os.environ.items()
           if k not in ("CODEX_COMPANION_PATH", "CODEX_COMPANION_ACCEPTS_EFFORT",
                        "CODEX_COMPANION_APP_SERVER_ENDPOINT")}
    env.update({"PATH": f"{bindir}:{os.environ['PATH']}", "CODEX_REVIEWS_DIR": str(reviews),
                "TMPDIR": str(inside), "REVIEW_BASE": "HEAD", "CODEX_OUTAGE_OVERRIDE": "1"})
    done = _run(["bash", str(REPO / "scripts/codex-dispatch.sh"), f"[review-kind: plan] {TODO} section 99\nReview it."],
                cwd=str(repo), env=env)
    assert done.returncode == 0, done.stdout[-2000:] + done.stderr[-2000:]
    turn = json.loads((bindir / "fake-codex-state.json").read_text()).get("lastTurnStart") or {}
    assert turn.get("effort") == "medium", (turn, done.stderr[-2000:])
    receipt = json.loads((reviews / "manifest.jsonl").read_text().splitlines()[-1])
    assert receipt.get("effort") == "medium" and receipt.get("schedule") == "effort", receipt
    assert receipt.get("pass_checked") is True and receipt.get("rc") == 0, receipt
    assert pathlib.Path(receipt["logFile"] + ".done").read_text().strip() == "0"
    assert not any(inside.iterdir()), list(inside.iterdir())
    assert not list(reviews.glob(".cp.*")), list(reviews.iterdir())
    for _ in range(40):
        if not _marked(str(reviews)):
            break
        time.sleep(0.05)
    assert not _marked(str(reviews)), "a process of the pass's companion outlived it"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
