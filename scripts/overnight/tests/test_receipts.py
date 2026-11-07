#!/usr/bin/env python3
"""Contract test for receipts.py content-addressed build receipts."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
RECEIPTS = HERE.parent / "receipts.py"


def _run(fx, verb):
    return subprocess.run([sys.executable, str(RECEIPTS), verb, str(fx)],
                          capture_output=True, text=True)


def test_receipt_lifecycle():
    with tempfile.TemporaryDirectory() as d:
        fx = pathlib.Path(d) / "fx"
        (fx / "src/kernel").mkdir(parents=True)
        (fx / "build").mkdir()
        subprocess.run(["git", "init", "-q", str(fx)], check=True)
        (fx / "src/kernel/a.c").write_text("int a(void){return 1;}\n")
        (fx / "Makefile").write_text("all:\n")
        subprocess.run(["git", "-C", str(fx), "add", "-A"], check=True)
        subprocess.run(["git", "-C", str(fx), "-c", "user.email=t@t",
                        "-c", "user.name=t", "commit", "-qm", "init"],
                       check=True, capture_output=True)

        # no receipt -> miss
        assert _run(fx, "check-build").returncode == 1
        # record -> valid
        assert _run(fx, "record-build").returncode == 0
        assert _run(fx, "check-build").returncode == 0
        # mtime-only touch of a tracked source -> STILL valid (content-bound)
        (fx / "src/kernel/a.c").touch()
        assert _run(fx, "check-build").returncode == 0
        # content edit -> miss
        (fx / "src/kernel/a.c").write_text("int a(void){return 2;}\n")
        r = _run(fx, "check-build")
        assert r.returncode == 1 and "changed" in r.stdout
        # revert to the receipted content -> valid again (no clock involved)
        (fx / "src/kernel/a.c").write_text("int a(void){return 1;}\n")
        assert _run(fx, "check-build").returncode == 0
        # new UNTRACKED build input -> miss; removing it -> valid
        (fx / "src/kernel/new.c").write_text("int n(void){return 0;}\n")
        assert _run(fx, "check-build").returncode == 1
        (fx / "src/kernel/new.c").unlink()
        assert _run(fx, "check-build").returncode == 0
        # non-build-input edits (docs/todo) never invalidate
        (fx / "NOTES.md").write_text("irrelevant\n")
        assert _run(fx, "check-build").returncode == 0


def test_smoke_receipt_binds_image_and_inputs():
    with tempfile.TemporaryDirectory() as d:
        fx = pathlib.Path(d) / "fx"
        (fx / "src/kernel").mkdir(parents=True)
        (fx / "build").mkdir()
        subprocess.run(["git", "init", "-q", str(fx)], check=True)
        (fx / "src/kernel/a.c").write_text("int a(void){return 1;}\n")
        (fx / "build/kernel.exe").write_bytes(b"IMAGEv1")
        subprocess.run(["git", "-C", str(fx), "add", "src/kernel/a.c"], check=True)
        subprocess.run(["git", "-C", str(fx), "-c", "user.email=t@t", "-c",
                        "user.name=t", "commit", "-qm", "c"], check=True,
                       capture_output=True)
        # no receipt -> miss
        assert _run(fx, "check-smoke").returncode == 1
        # record -> valid
        assert _run(fx, "record-smoke").returncode == 0
        assert _run(fx, "check-smoke").returncode == 0
        # image changes (rebuilt/clobbered) but inputs same -> MISS (image-bound)
        (fx / "build/kernel.exe").write_bytes(b"IMAGEv2-different")
        r = _run(fx, "check-smoke")
        assert r.returncode == 1 and "image changed" in r.stdout, r.stdout
        # restore image -> valid again (content-addressed, no clock)
        (fx / "build/kernel.exe").write_bytes(b"IMAGEv1")
        assert _run(fx, "check-smoke").returncode == 0
        # a build-input content change -> MISS
        (fx / "src/kernel/a.c").write_text("int a(void){return 2;}\n")
        assert _run(fx, "check-smoke").returncode == 1


def test_record_rollover_records_all_three_consistently():
    # J1: record-rollover records build + suite 'all' + smoke over the SAME tree
    # in one shot, so all three check-valid together (no order-sensitive cascade).
    with tempfile.TemporaryDirectory() as d:
        fx = pathlib.Path(d) / "fx"
        (fx / "src/kernel").mkdir(parents=True)
        (fx / "build").mkdir()
        subprocess.run(["git", "init", "-q", str(fx)], check=True)
        (fx / "src/kernel/a.c").write_text("int a(void){return 1;}\n")
        (fx / "Makefile").write_text("all:\n")
        (fx / "build/kernel.exe").write_bytes(b"IMAGEv1")
        subprocess.run(["git", "-C", str(fx), "add", "-A"], check=True)
        subprocess.run(["git", "-C", str(fx), "-c", "user.email=t@t",
                        "-c", "user.name=t", "commit", "-qm", "init"],
                       check=True, capture_output=True)
        # REFUSAL: no verdicts at all records nothing.
        assert _run(fx, "record-rollover").returncode == 1
        assert _run(fx, "check-build").returncode == 1
        _verdicts(fx)
        assert _run(fx, "record-rollover").returncode == 0
        # all three receipts valid over the same final tree/image
        assert _run(fx, "check-build").returncode == 0
        r = subprocess.run([sys.executable, str(RECEIPTS), "check-suite",
                            str(fx), "all"], capture_output=True, text=True)
        assert r.returncode == 0, r.stdout
        assert _run(fx, "check-smoke").returncode == 0


def _verdicts(fx, **over):
    sys.path.insert(0, str(HERE.parent))
    import receipts
    key = receipts.build_input_key(fx)
    image = receipts.image_key(fx)
    data = {label: {"state": "complete", "exit": 0, "epoch": 100 + i,
                    "build_input_key": key, "image_key": image}
            for i, label in enumerate(("j1-build", "j1-test", "j1-smoke"))}
    for label, change in over.items():
        data[label.replace("_", "-")].update(change)
    (fx / ".claude/state/artifact-verdicts").mkdir(parents=True, exist_ok=True)
    for label, record in data.items():
        (fx / f".claude/state/artifact-verdicts/{label}.json").write_text(json.dumps(record))


def test_record_rollover_refuses_a_red_or_unfinished_j1():
    # v22: `FAIL: 1 of 35938` was followed by `rollover receipt set recorded`.
    with tempfile.TemporaryDirectory() as d:
        fx = pathlib.Path(d) / "fx"
        (fx / "src/kernel").mkdir(parents=True)
        (fx / "build").mkdir()
        subprocess.run(["git", "init", "-q", str(fx)], check=True)
        (fx / "src/kernel/a.c").write_text("int a(void){return 1;}\n")
        (fx / "Makefile").write_text("all:\n")
        (fx / "build/kernel.exe").write_bytes(b"IMAGEv1")
        subprocess.run(["git", "-C", str(fx), "add", "-A"], check=True)
        subprocess.run(["git", "-C", str(fx), "-c", "user.email=t@t",
                        "-c", "user.name=t", "commit", "-qm", "init"],
                       check=True, capture_output=True)
        for over in ({"j1_test": {"exit": 1}},
                     {"j1_build": {"exit": 2}},
                     {"j1_smoke": {"state": "running", "exit": None}},
                     {"j1_test": {"build_input_key": "other"}},
                     {"j1_smoke": {"epoch": 1}},
                     {"j1_smoke": {"image_key": "an image nothing booted"}}):
            _verdicts(fx, **over)
            r = _run(fx, "record-rollover")
            assert r.returncode == 1, (over, r.stdout)
            assert "NOT recorded" in r.stderr, (over, r.stderr)
            assert _run(fx, "check-build").returncode == 1, over
        _verdicts(fx)
        # REFUSAL, review round 6: the image changes after a green chain.
        (fx / "build/kernel.exe").write_bytes(b"IMAGEv2")
        r = _run(fx, "record-rollover")
        assert r.returncode == 1 and "image changed" in r.stderr, r.stderr
        _verdicts(fx)
        assert _run(fx, "record-rollover").returncode == 0


def test_run_artifact_writes_the_verdict_record_cannot_stay_green():
    script = HERE.parent / "run-artifact.sh"
    with tempfile.TemporaryDirectory() as d:
        fx = pathlib.Path(d)
        subprocess.run(["git", "init", "-q", str(fx)], check=True)
        verdict = fx / ".claude/state/artifact-verdicts/j1-test.json"
        subprocess.run(["bash", str(script), "j1-test", "--", "true"], cwd=fx,
                       capture_output=True)
        got = json.loads(verdict.read_text())
        assert got["exit"] == 0 and got["state"] == "complete"
        subprocess.run(["bash", str(script), "j1-test", "--", "false"], cwd=fx,
                       capture_output=True)
        assert json.loads(verdict.read_text())["exit"] == 1
        # REFUSAL, review round 7: another label's run must not touch this verdict,
        # and a killed run must not leave its previous green behind.
        subprocess.run(["bash", str(script), "other", "--", "true"], cwd=fx,
                       capture_output=True)
        assert json.loads(verdict.read_text())["exit"] == 1
        subprocess.run(["bash", str(script), "j1-test", "--", "true"], cwd=fx,
                       capture_output=True)
        subprocess.run(["bash", str(script), "j1-test", "--", "bash", "-c", "kill -9 $PPID"],
                       cwd=fx, capture_output=True)
        assert json.loads(verdict.read_text())["state"] == "running"
        # REFUSAL, review round 8: an old green that cannot be cleared stops the leg.
        if os.geteuid() != 0:
            subprocess.run(["bash", str(script), "j1-test", "--", "true"], cwd=fx,
                           capture_output=True)
            os.chmod(verdict.parent, 0o555)
            try:
                marker = fx / "ran"
                r = subprocess.run(["bash", str(script), "j1-test", "--", "touch", str(marker)],
                                   cwd=fx, capture_output=True, text=True)
                assert r.returncode == 3 and not marker.exists(), r
                # Other labels stay best-effort, as the wrapper has always been.
                r = subprocess.run(["bash", str(script), "plain", "--", "true"], cwd=fx,
                                   capture_output=True, text=True)
                assert r.returncode == 0, r
            finally:
                os.chmod(verdict.parent, 0o755)


if __name__ == "__main__":
    test_record_rollover_refuses_a_red_or_unfinished_j1()
    test_run_artifact_writes_the_verdict_record_cannot_stay_green()
    test_receipt_lifecycle()
    test_smoke_receipt_binds_image_and_inputs()
    test_record_rollover_records_all_three_consistently()
    print("PASS: receipts")
