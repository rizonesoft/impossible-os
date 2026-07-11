#!/usr/bin/env python3
"""Contract test for receipts.py content-addressed build receipts."""
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


if __name__ == "__main__":
    test_receipt_lifecycle()
    test_smoke_receipt_binds_image_and_inputs()
    print("PASS: receipts")
