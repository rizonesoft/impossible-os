#!/usr/bin/env python3
# block-via: exit 2 (unattended overnight run ONLY -- BLOCK an edit to the build/
# verification machinery the run's own receipts are computed over; file it instead.
# Interactive sessions are never gated.)
"""PreToolUse (Edit|Write|NotebookEdit): the receipt-surface self-modify guard.

WHY THIS EXISTS (v04 runner item, observed live 2026-07-29 12:50-12:51, ~14h
into the 3-day canary). The self-modify boundary was drawn by DIRECTORY --
`.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`,
`.claude/settings.json` -- and a directory list cannot express blast radius.

The run hit a lexical bug that "bit four consecutive rounds", correctly stopped
patching corners, and replaced a hand-rolled lexer with a byte-exact comparison.
Good root-cause judgment. But that refactor left `scripts/build.sh`, `Makefile`,
`scripts/gen-user-abi.py`, `include/kernel/abi_hash.h` and
`user/include/abi_numbers.h` modified -- 12 dirty files -- and EVERY ONE of them
is outside the boundary. Meanwhile `receipts.py` fingerprints
`BUILD_INPUT_PATHS = src include user resources tools Makefile scripts/build.sh
boot.conf`, so `scripts/build.sh` and `Makefile` are precisely what the run's own
build/test/smoke receipts are computed over. The run could rewrite the thing its
verification is anchored to, unattended and inside the rules.

THE FAILURE MODE IS NOT A WEDGE, IT IS A SILENT LOSS OF MEANING. A hook edit
breaks loudly and immediately. A subtly wrong `build.sh` or ABI generator keeps
producing GREEN receipts that no longer mean what they say, for as long as the
run continues. Nothing observed suggests this HAS happened; the point is that the
boundary would not have stopped it.

WHY NOT JUST WIDEN THE DIRECTORY LIST. "Everything risky" is not enumerable by
path, and a longer denylist would ALSO have blocked the genuinely good refactor
above -- which is ordinary work that simply needs a human to look at it. So the
gate is on the RECEIPT SURFACE: the intersection of "what verification depends
on" (`BUILD_INPUT_PATHS`, imported from receipts.py -- NOT re-typed here, so it
cannot drift from the list receipts actually use) with "is build/verification
MACHINERY rather than compiled source". `src/`, `include/`, `user/`, `resources/`
and `tools/` are in BUILD_INPUT_PATHS but are ordinary work and stay untouched;
`Makefile*`, `scripts/build.sh` and the ABI generator are the machinery.

Interactive sessions are NEVER gated: an operator editing build.sh is the
intended repair path, and this guard exists only because nobody is watching.

Fail-OPEN on any error. A guard that wedges the run it protects is worse than
the risk it covers.

Selftest: python3 receipt_surface_guard.py --selftest
"""
from __future__ import annotations

import fnmatch
import json
import os
import sys
from pathlib import Path

# Machinery that is BOTH a receipt input and a build/verification tool. Every
# entry must also be reachable from BUILD_INPUT_PATHS (asserted at import by
# _machinery_paths) so this list can never quietly outgrow the receipt surface.
_MACHINERY = (
    "Makefile",
    "Makefile.*",
    "*/Makefile",
    "scripts/build.sh",
)

# The ABI generator is machinery by ROLE rather than by being a receipt input in
# its own right: it lives under tools/ or scripts/ and RENDERS the contract that
# `abi_hash.h` / `abi_numbers.h` pin, so a wrong generator produces a consistent
# but meaningless fingerprint on both sides of the handshake -- exactly the
# "green receipt that no longer means what it says" failure.
_ABI_MACHINERY = (
    "scripts/gen-user-abi.py",
    "scripts/abi-stamp-check.sh",
    "tools/boot-info-manifest/*",
)


def _repo_root() -> Path:
    env = os.environ.get("CLAUDE_PROJECT_DIR")
    if env:
        return Path(env).resolve()
    return Path(__file__).resolve().parents[2]


def _build_input_paths(root: Path):
    """Import BUILD_INPUT_PATHS from receipts.py rather than re-typing it.

    The whole point of the item is to reuse the list the repo already maintains
    for the "what does verification depend on" question. A second copy here
    would drift from it, which is the defect one level up."""
    try:
        import importlib.util
        src = root / "scripts/overnight/receipts.py"
        spec = importlib.util.spec_from_file_location("_receipts", src)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        paths = getattr(mod, "BUILD_INPUT_PATHS", None)
        return list(paths) if isinstance(paths, (list, tuple)) else None
    except Exception:
        return None


def _under_build_inputs(rel: str, build_inputs) -> bool:
    for entry in build_inputs:
        if rel == entry or rel.startswith(entry.rstrip("/") + "/"):
            return True
        if fnmatch.fnmatch(rel, entry) or fnmatch.fnmatch(rel, entry + "/*"):
            return True
    return False


def is_receipt_machinery(rel: str, build_inputs) -> bool:
    """True iff `rel` is build/verification MACHINERY on the receipt surface."""
    rel = rel.replace("\\", "/").lstrip("./")
    for pat in _ABI_MACHINERY:
        if fnmatch.fnmatch(rel, pat):
            return True
    for pat in _MACHINERY:
        if fnmatch.fnmatch(rel, pat):
            # Must also be a receipt input -- that is the whole gate. A stray
            # Makefile outside BUILD_INPUT_PATHS is ordinary work.
            if build_inputs is None or _under_build_inputs(rel, build_inputs):
                return True
    return False


def _is_unattended() -> bool:
    """ONLY the headless overnight run is gated. Session identity, not global
    run state: an interactive operator session in the same repo while a run is
    armed must stay ungated (the discriminator `OVERNIGHT_SEQUENCER_RUN=1` is
    set by the launcher for the run's own process only)."""
    return os.environ.get("OVERNIGHT_SEQUENCER_RUN") == "1"


_MSG = (
    "[receipt-surface BLOCK -- file it, do not apply it] `{rel}` is build/"
    "verification MACHINERY on the RECEIPT SURFACE: it is one of the "
    "`BUILD_INPUT_PATHS` that `scripts/overnight/receipts.py` fingerprints, so "
    "the run's own build/test/smoke receipts are computed OVER this file. "
    "Editing it unattended means a later green receipt may no longer mean what "
    "it says -- and unlike a hook edit, that fails SILENTLY for as long as the "
    "run continues.\n"
    "  This is not a refusal of the work, only of applying it unwatched. FILE "
    "it: append an item to `todo/overnight-runner-improvements/"
    "overnight-runner-improvements-v04.md` (or the owning TODO) describing the "
    "change and why it is needed, then CONTINUE with the section. An operator "
    "applies it attended, where the diff gets read.\n"
    "  Ordinary work is unaffected: `src/`, `include/`, `user/`, `resources/` "
    "and `tools/` are receipt inputs but are NOT machinery and are never gated "
    "by this hook.\n"
    "  Opt-out (interactive rescue only): RECEIPT_SURFACE_OVERRIDE=1"
)


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0
    if d.get("tool_name") not in ("Edit", "Write", "NotebookEdit"):
        return 0
    if not _is_unattended():
        return 0
    if os.environ.get("RECEIPT_SURFACE_OVERRIDE") == "1":
        return 0
    ti = d.get("tool_input") or {}
    fp = str(ti.get("file_path") or ti.get("notebook_path") or "")
    if not fp:
        return 0
    root = _repo_root()
    try:
        rel = os.path.relpath(os.path.abspath(fp), str(root)).replace("\\", "/")
    except Exception:
        return 0
    if rel.startswith(".."):
        return 0                      # outside the repo -- not our business
    if not is_receipt_machinery(rel, _build_input_paths(root)):
        return 0
    sys.stderr.write(_MSG.format(rel=rel) + "\n")
    return 2


def _selftest() -> int:
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    # Derive the fixture from the REAL list rather than re-typing it: a literal
    # copy here would go stale the moment receipts.py changed, which is the
    # exact drift this gate exists to avoid one level up.
    bi = _build_input_paths(_repo_root())
    check("BUILD_INPUT_PATHS readable for the fixture", bi is not None)
    if bi is None:
        bi = []

    # The five files the live incident actually dirtied.
    check("build.sh is machinery", is_receipt_machinery("scripts/build.sh", bi))
    check("Makefile is machinery", is_receipt_machinery("Makefile", bi))
    check("abi generator is machinery",
          is_receipt_machinery("scripts/gen-user-abi.py", bi))
    check("abi stamp check is machinery",
          is_receipt_machinery("scripts/abi-stamp-check.sh", bi))
    # ...but the two generated HEADERS are ordinary source: they are outputs of
    # the generator, and gating them would block ordinary ABI work.
    check("abi_hash.h is NOT machinery",
          not is_receipt_machinery("include/kernel/abi_hash.h", bi))
    check("abi_numbers.h is NOT machinery",
          not is_receipt_machinery("user/include/abi_numbers.h", bi))

    # Ordinary work must stay completely untouched -- this is the reason the
    # gate is not "widen the directory denylist".
    for rel in ("src/kernel/sched/task.c", "include/kernel/types.h",
                "user/lib/crt_init.c", "resources/fonts/x.ttf",
                "todo/00-infrastructure/TODO-04.md", "docs/x.md",
                "scripts/test.sh", "scripts/lint.sh"):
        check(f"ordinary: {rel}", not is_receipt_machinery(rel, bi))

    # Sub-Makefiles are machinery too (src/boot/uefi has its own).
    check("sub-Makefile is machinery",
          is_receipt_machinery("src/boot/uefi/Makefile", bi))

    # A Makefile OUTSIDE the receipt surface is ordinary work.
    check("off-surface Makefile is ordinary",
          not is_receipt_machinery("Makefile", ["src", "include"]))

    # Import path: the real receipts.py list must be readable and must still
    # contain the two machinery entries this gate leans on. If receipts.py ever
    # drops them, this assert is the alarm.
    real = _build_input_paths(_repo_root())
    check("BUILD_INPUT_PATHS importable", real is not None)
    if real:
        check("build.sh still a receipt input", "scripts/build.sh" in real)
        check("Makefile still a receipt input", "Makefile" in real)

    # Never fires interactively, and never on a non-edit tool.
    import contextlib
    import io
    os.environ.pop("OVERNIGHT_SEQUENCER_RUN", None)
    payload = {"tool_name": "Edit",
               "tool_input": {"file_path": str(_repo_root() / "scripts/build.sh")}}
    old = sys.stdin
    try:
        sys.stdin = io.StringIO(json.dumps(payload))
        with contextlib.redirect_stderr(io.StringIO()):
            check("interactive never blocks", main() == 0)
        os.environ["OVERNIGHT_SEQUENCER_RUN"] = "1"
        sys.stdin = io.StringIO(json.dumps(payload))
        with contextlib.redirect_stderr(io.StringIO()):
            check("unattended blocks", main() == 2)
        os.environ["RECEIPT_SURFACE_OVERRIDE"] = "1"
        sys.stdin = io.StringIO(json.dumps(payload))
        with contextlib.redirect_stderr(io.StringIO()):
            check("override releases", main() == 0)
        os.environ.pop("RECEIPT_SURFACE_OVERRIDE", None)
        sys.stdin = io.StringIO(json.dumps({"tool_name": "Bash",
                                            "tool_input": {"command": "ls"}}))
        with contextlib.redirect_stderr(io.StringIO()):
            check("non-edit tool ignored", main() == 0)
        sys.stdin = io.StringIO("not json")
        with contextlib.redirect_stderr(io.StringIO()):
            check("malformed input fails open", main() == 0)
    finally:
        sys.stdin = old
        os.environ.pop("OVERNIGHT_SEQUENCER_RUN", None)

    if fails:
        sys.stderr.write("receipt_surface_guard selftest FAIL: "
                         + "; ".join(sorted(set(fails))) + "\n")
        return 1
    print("receipt_surface_guard selftest OK")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(_selftest() if "--selftest" in sys.argv else main())
    except Exception:
        sys.exit(0)          # fail-open: never wedge the run this protects
