#!/usr/bin/env python3
"""CI runs in the pinned image, and the image's toolchain is installed once, by
scripts/ci/toolchain.sh, when the image is built.

Each rule here was paid for: `ubuntu-latest` moved to 26.04 without a commit and that
image had no apt-key, so the old apt.llvm.org setup failed every workflow; a stalled
mirror used whole step timeouts four times on 2026-11-10; nine Build runs in a row
failed in their install step on 2026-11-11. Since the jobs moved into the image no job installs
anything (scripts/ci/check-pinned-image.py), so these hold the one install left.
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

import yaml

REPO = pathlib.Path(__file__).resolve().parents[3]
CI = REPO / "scripts/ci"
TOOLCHAIN = (CI / "toolchain.sh").read_text()
HELPER = (CI / "apt-install.sh").read_text()


def manifest():
    llvm = re.search(r"^LLVM_VERSION=(\d+)$", TOOLCHAIN, re.M).group(1)
    out = []
    for line in (CI / "toolchain-packages.txt").read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.append(line.replace("@LLVM@", llvm))
    return out


def test_the_image_holds_the_pinned_toolchain():
    pkgs = manifest()
    assert len(pkgs) == len(set(pkgs)), "a package listed twice"
    for need in ("clang-19", "lld-19", "llvm-19", "qemu-system-x86", "python3-pytest", "zip", "bear"):
        assert need in pkgs, need
    for path in (REPO / ".github/workflows").glob("*.yml"):
        assert "apt-key" not in path.read_text(), path.name


def test_the_install_is_bounded_and_retried():
    """Every package goes through apt-install.sh (bounded download, retried, a fallback
    mirror). The one plain apt-get install is GNU coreutils, which the helper needs."""
    plain = re.findall(r"^apt-get install .*$", TOOLCHAIN, re.M)
    assert len(plain) == 1 and "coreutils-from-gnu coreutils-from-uutils-" in plain[0], plain
    assert re.search(r'^\S.*bash "\$HERE/apt-install\.sh" "\$\{PACKAGES\[@\]\}"$', TOOLCHAIN, re.M)
    assert "--download-only" in HELPER and "--no-download" in HELPER and "timeout -k" in HELPER
    assert "FALLBACK_MIRROR" in HELPER, "a retry must be able to leave a mirror this runner cannot reach"
    assert "-qq" not in TOOLCHAIN, "keep apt output in the image build log"


def worst_case_seconds():
    cap = {k: int(v) for k, v in re.findall(r"\$\{APT_(\w+):-(\d+)\}", HELPER)}
    cap.update({k: int(v) for k, v in re.findall(r"\bAPT_(\w+)=(\d+)\b", TOOLCHAIN)})
    grace = int(re.search(r"timeout -k (\d+)", HELPER).group(1))
    n = cap["INSTALL_ATTEMPTS"]
    pauses = sum(i * 5 for i in range(1, n))
    return n * (cap["UPDATE_SECONDS"] + cap["DOWNLOAD_SECONDS"] + 2 * grace) + pauses + cap["INSTALL_SECONDS"] + grace


def test_the_worst_case_install_fits_the_publishing_job():
    """A job timeout shorter than the helper's worst case cancels it mid-recovery (Codex,
    2026-11-10). The image build is the only install, so its job must outlast it, with
    ten minutes left for the coreutils swap, the checks and the push."""
    job = yaml.safe_load((REPO / ".github/workflows/ci-image.yml").read_text())["jobs"]["publish"]
    assert job["timeout-minutes"] * 60 >= worst_case_seconds() + 600, (job["timeout-minutes"], worst_case_seconds())


def test_gnu_coreutils_come_first():
    """uutils `timeout -s KILL` leaves a stalled child running, so the helper's bounds and
    the identity gate's fail. The swap precedes every other install."""
    assert TOOLCHAIN.index("coreutils-from-gnu") < TOOLCHAIN.index('bash "$HERE/apt-install.sh"')
    assert "timeout --version | grep -q 'GNU coreutils'" in TOOLCHAIN


def test_setup_verify_refuses_a_non_gnu_timeout():
    with tempfile.TemporaryDirectory() as d:
        fake = pathlib.Path(d, "timeout")
        fake.write_text("#!/bin/sh\necho 'timeout (uutils coreutils) 0.10.0'\n")
        fake.chmod(0o755)
        env = dict(os.environ, PATH=f"{d}:{os.environ['PATH']}")
        r = subprocess.run(["bash", "scripts/setup.sh", "--verify"], cwd=REPO, env=env,
                           capture_output=True, text=True, timeout=120)
    assert r.returncode != 0 and "GNU timeout" in r.stdout, (r.returncode, r.stdout[-300:])
    r = subprocess.run(["bash", "scripts/setup.sh", "--verify"], cwd=REPO, capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, ("a GNU host must verify clean", r.stdout[-400:])


def test_no_workflow_follows_a_floating_image():
    for path in (REPO / ".github/workflows").glob("*.yml"):
        text = path.read_text()
        assert "-latest" not in "".join(re.findall(r"runs-on:.*", text)), path.name
        assert re.findall(r"runs-on:\s*ubuntu-\d", text), (path.name, "no pinned image")
    base = re.search(r"^FROM (\S+)$", (CI / "image/Dockerfile").read_text(), re.M).group(1)
    assert re.fullmatch(r"ubuntu:\d+\.\d+@sha256:[0-9a-f]{64}", base), base



def test_the_published_qemu_matches_the_record_exactly():
    """A substring test accepted 10.2.10 for a recorded 10.2.1 (Codex, 2026-11-13),
    and the CI-parity gate would then compare the local QEMU with a stale record."""
    with tempfile.TemporaryDirectory() as d:
        rec = pathlib.Path(d, "v.txt")
        rec.write_text("# the CI image's QEMU\n10.2.1\n")
        def check(out):
            return subprocess.run([sys.executable, str(CI / "check-qemu-version.py"), str(rec)],
                                  input=out, capture_output=True, text=True, timeout=30).returncode
        assert check("QEMU emulator version 10.2.1 (Debian 1:10.2.1+ds-1ubuntu1)\n") == 0
        assert check("QEMU emulator version 10.2.1\n") == 0
        for bad in ("QEMU emulator version 10.2.10\n", "QEMU emulator version 10.2.1.5\n",
                    "QEMU emulator version 10.2.0\n", "", "garbage\n"):
            assert check(bad) == 1, bad
        rec.write_text("# nothing recorded\n")
        assert check("QEMU emulator version 10.2.1\n") == 1
    wf = (REPO / ".github/workflows/ci-image.yml").read_text()
    assert "| python3 scripts/ci/check-qemu-version.py" in wf
    assert "scripts/ci/check-qemu-version.py" in yaml.safe_load(wf)[True]["push"]["paths"]


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("PASS test_ci_toolchain_install")
    sys.exit(0)
