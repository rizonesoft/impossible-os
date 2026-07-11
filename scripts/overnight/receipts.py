#!/usr/bin/env python3
"""Content-addressed evidence receipts for the overnight pipeline.

Replaces wall-clock evidence expiry (the section-commit gate's 30-min TTLs)
with content binding: a build or review receipt stays valid for as long as
the content it attests to -- build inputs, configuration, and toolchain --
is byte-identical, and dies the moment any of it changes. That is strictly
stronger than a TTL (a 29-min-old receipt over changed content was never
valid; a 3-hour-old receipt over unchanged content always was) and stops
slow external reviews from forcing expensive build/test/review reruns.

Two fingerprints:

  build_input_key(project)   sha256 over the git state of the BUILD INPUT
                             paths only (src/ include/ user/ resources/
                             tools/ Makefile* scripts/build.sh linker
                             scripts): HEAD tree entries + worktree diff +
                             untracked file content hashes. TODO/doc edits
                             after a build do NOT invalidate it; any source
                             edit does.
  toolchain_key()            sha256 over the version banners of clang-19,
                             nasm, ld.lld-19.

CLI (called by scripts/build.sh and the section-commit gate):

  receipts.py record-build PROJECT_DIR   write build/build-receipt.json
                                         (call ONLY after === BUILD OK ===)
  receipts.py check-build PROJECT_DIR    exit 0 iff the receipt matches the
                                         CURRENT tree + toolchain

Stdlib only; importable as a module by hooks (sys.path insert).
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

# Paths whose content feeds the kernel build. scripts/ is deliberately NOT
# included wholesale -- runner/tooling scripts change without affecting the
# compiled image; build.sh itself and the Makefiles are the build-config
# surface that must invalidate.
BUILD_INPUT_PATHS = [
    "src", "include", "user", "resources", "tools",
    "Makefile", "scripts/build.sh", "boot.conf",
]

TOOLCHAIN_CMDS = [
    ["clang-19", "--version"],
    ["nasm", "-v"],
    ["ld.lld-19", "--version"],
]


def _git(project: Path, *args: str, timeout: int = 60) -> str | None:
    try:
        r = subprocess.run(["git", "-C", str(project), *args],
                           capture_output=True, timeout=timeout)
        return r.stdout.decode("utf-8", "replace") if r.returncode == 0 else None
    except Exception:
        return None


def build_input_key(project: Path) -> str | None:
    """Content fingerprint of everything the build reads. None on git failure
    (callers fail toward 'no receipt', never toward a false pass)."""
    head_tree = _git(project, "ls-tree", "-r", "HEAD", "--", *BUILD_INPUT_PATHS)
    diff = _git(project, "diff", "HEAD", "--", *BUILD_INPUT_PATHS)
    untracked = _git(project, "ls-files", "-o", "--exclude-standard", "--",
                     *BUILD_INPUT_PATHS)
    if head_tree is None or diff is None or untracked is None:
        return None
    h = hashlib.sha256()
    h.update(head_tree.encode())
    h.update(diff.encode())
    # Untracked build inputs: bind their CONTENT, not just their names, via
    # git hash-object (a new .c file edited after the build must invalidate).
    names = [n for n in untracked.splitlines() if n.strip()]
    h.update("\n".join(names).encode())
    if names:
        try:
            r = subprocess.run(
                ["git", "-C", str(project), "hash-object", "--stdin-paths"],
                input="\n".join(names).encode(), capture_output=True, timeout=120)
            if r.returncode != 0:
                return None
            h.update(r.stdout)
        except Exception:
            return None
    return h.hexdigest()


def toolchain_key() -> str:
    """Fingerprint of the toolchain version banners. A missing tool hashes as
    its absence -- receipt comparisons still work, they just bind to 'absent'."""
    h = hashlib.sha256()
    for cmd in TOOLCHAIN_CMDS:
        try:
            r = subprocess.run(cmd, capture_output=True, timeout=15)
            first = (r.stdout or r.stderr).decode("utf-8", "replace").splitlines()
            h.update((cmd[0] + ":" + (first[0] if first else "")).encode())
        except Exception:
            h.update((cmd[0] + ":absent").encode())
    return h.hexdigest()


# Built artifacts a smoke test actually boots. The smoke receipt binds to their
# CONTENT (not just the build inputs) so a stale image can never pass as green.
IMAGE_PATHS = ["build/kernel.exe", "build/BOOTX64.EFI", "build/impossible.img",
               "build/disk.img", "build/esp.img", "build/impossible-os.img"]
# Markers that prove a green boot-to-userspace (test-smoke.sh success condition).
SMOKE_MARKERS_DEFAULT = "Boot complete;C:\\>"


def image_key(project: Path) -> str:
    """Fingerprint of the built image artifacts. 'no-image' when none exist."""
    h = hashlib.sha256()
    found = False
    for rel in IMAGE_PATHS:
        p = project / rel
        try:
            data = p.read_bytes()
        except OSError:
            continue
        found = True
        h.update(rel.encode())
        h.update(hashlib.sha256(data).digest())
    return h.hexdigest() if found else "no-image"


def receipt_path(project: Path) -> Path:
    return project / "build" / "build-receipt.json"


def record_build(project: Path) -> int:
    key = build_input_key(project)
    if key is None:
        print("receipts: git unavailable, build receipt not recorded",
              file=sys.stderr)
        return 1
    rp = receipt_path(project)
    rp.parent.mkdir(parents=True, exist_ok=True)
    rp.write_text(json.dumps({
        "build_input_key": key,
        "toolchain_key": toolchain_key(),
        "epoch": int(time.time()),
    }))
    print(f"build receipt recorded ({key[:12]})")
    return 0


def check_build(project: Path) -> tuple[bool, str]:
    """(valid, reason). Valid iff a receipt exists and both fingerprints
    match the CURRENT tree/toolchain -- age is irrelevant by design."""
    rp = receipt_path(project)
    try:
        rec = json.loads(rp.read_text())
    except (OSError, ValueError):
        return False, "no build receipt (build/build-receipt.json missing or unreadable)"
    key = build_input_key(project)
    if key is None:
        return False, "git unavailable for fingerprinting"
    if rec.get("build_input_key") != key:
        return False, "build inputs changed since the receipted build"
    if rec.get("toolchain_key") != toolchain_key():
        return False, "toolchain changed since the receipted build"
    return True, f"receipt matches current build inputs ({key[:12]})"


def suite_receipts_path(project: Path) -> Path:
    return project / "build" / "suite-receipts.json"


def record_suite(project: Path, suite: str) -> int:
    """Record a GREEN run of one test suite (e.g. mm, ob, exec) bound to the
    current build-input + toolchain fingerprints. Enables targeted fix-loop
    verification: re-running an owning suite over unchanged content is free."""
    key = build_input_key(project)
    if key is None:
        print("receipts: git unavailable, suite receipt not recorded",
              file=sys.stderr)
        return 1
    p = suite_receipts_path(project)
    try:
        data = json.loads(p.read_text())
    except (OSError, ValueError):
        data = {}
    data[suite] = {"build_input_key": key, "toolchain_key": toolchain_key(),
                   "epoch": int(time.time())}
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(data))
    print(f"suite receipt recorded: {suite} ({key[:12]})")
    return 0


def check_suite(project: Path, suite: str) -> tuple[bool, str]:
    try:
        rec = json.loads(suite_receipts_path(project).read_text()).get(suite)
    except (OSError, ValueError):
        rec = None
    if not rec:
        return False, f"no receipt for suite {suite}"
    key = build_input_key(project)
    if key is None:
        return False, "git unavailable for fingerprinting"
    if rec.get("build_input_key") != key:
        return False, "build inputs changed since the receipted suite run"
    if rec.get("toolchain_key") != toolchain_key():
        return False, "toolchain changed since the receipted suite run"
    return True, f"suite {suite} green over current inputs ({key[:12]})"


def smoke_receipt_path(project: Path) -> Path:
    return project / "build" / "smoke-receipt.json"


def record_smoke(project: Path, markers: str = SMOKE_MARKERS_DEFAULT) -> int:
    """Record a GREEN smoke run bound to build inputs + toolchain + the built
    IMAGE bytes + the success markers. Stronger than a suite receipt: it also
    invalidates when the image changes, even if the tracked inputs somehow did
    not (a rebuilt or partially-clobbered image)."""
    key = build_input_key(project)
    if key is None:
        print("receipts: git unavailable, smoke receipt not recorded",
              file=sys.stderr)
        return 1
    rp = smoke_receipt_path(project)
    rp.parent.mkdir(parents=True, exist_ok=True)
    img = image_key(project)
    rp.write_text(json.dumps({
        "build_input_key": key, "toolchain_key": toolchain_key(),
        "image_key": img, "markers": markers, "epoch": int(time.time())}))
    print(f"smoke receipt recorded ({key[:12]}, image {img[:12]})")
    return 0


def check_smoke(project: Path, markers: str = SMOKE_MARKERS_DEFAULT
                ) -> tuple[bool, str]:
    """(valid, reason). Valid iff build inputs, toolchain, built image, AND the
    expected markers all match the receipt -- age irrelevant by design."""
    try:
        rec = json.loads(smoke_receipt_path(project).read_text())
    except (OSError, ValueError):
        return False, "no smoke receipt (record with receipts.py record-smoke .)"
    key = build_input_key(project)
    if key is None:
        return False, "git unavailable for fingerprinting"
    if rec.get("build_input_key") != key:
        return False, "build inputs changed since the receipted smoke"
    if rec.get("toolchain_key") != toolchain_key():
        return False, "toolchain changed since the receipted smoke"
    if rec.get("image_key") != image_key(project):
        return False, "built image changed since the receipted smoke"
    if markers and rec.get("markers") != markers:
        return False, "expected smoke markers differ from the receipt"
    return True, f"smoke green over current inputs+image ({key[:12]})"


def main(argv: list[str]) -> int:
    verbs = ("record-build", "check-build", "record-suite", "check-suite",
             "record-smoke", "check-smoke")
    if len(argv) < 2 or argv[0] not in verbs:
        print("usage: receipts.py record-build|check-build PROJECT_DIR | "
              "record-suite|check-suite PROJECT_DIR SUITE | "
              "record-smoke|check-smoke PROJECT_DIR", file=sys.stderr)
        return 2
    project = Path(argv[1])
    if argv[0] == "record-build":
        return record_build(project)
    if argv[0] == "check-build":
        ok, reason = check_build(project)
        print(("VALID: " if ok else "MISS: ") + reason)
        return 0 if ok else 1
    if argv[0] == "record-smoke":
        return record_smoke(project)
    if argv[0] == "check-smoke":
        ok, reason = check_smoke(project)
        print(("VALID: " if ok else "MISS: ") + reason)
        return 0 if ok else 1
    if len(argv) < 3:
        print("suite verbs need a SUITE name", file=sys.stderr)
        return 2
    if argv[0] == "record-suite":
        return record_suite(project, argv[2])
    ok, reason = check_suite(project, argv[2])
    print(("VALID: " if ok else "MISS: ") + reason)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
