#!/usr/bin/env python3
"""Kernel BSS-to-USER_BASE headroom sensor.

WHY THIS EXISTS. `scripts/build.sh` already refuses a build whose kernel `.bss`
end has crossed `USER_BASE` ("BSS COLLISION"), but it can only say so AFTER a
full compile -- which is after the design review, the implementation and the
tests are already spent. `01-boot-platform/TODO-13` section 28 was reverted
whole for exactly this, and `02-kernel-core/TODO-33` section 7 lists re-run
items for TODO-22 sections 23/24/25 and TODO-23 sections 1-16 parked the same
way, so the cost has been paid at least five times.

The overshoot is CASCADING, not proportional -- `.rodata`, `.data` and `.bss`
each `ALIGN(4K)` and chain -- so a section either fits with room to spare or
overshoots by a whole page, and no amount of care during implementation
predicts it. Measured at `d43b73d2b`: 347 bytes of headroom, and 859 bytes of
`.rodata` growth was enough to trip it.

This reads a file the build ALREADY produced (`build/kernel.map`), so it costs
no model tokens and no extra build. Advisory by construction: it never fails a
build, it reports headroom so a section that adds kernel `.text`/`.rodata` can
park its carriage EARLY instead of discovering the ceiling twice.

  python3 scripts/overnight/bss-headroom.py            # JSON verdict
  python3 scripts/overnight/bss-headroom.py --selftest
"""
import json
import re
import sys
from pathlib import Path

# One page of slack. Below this a section that adds any kernel .text/.rodata is
# a coin flip, because a single ALIGN(4K) boundary is the whole margin.
TIGHT_BYTES = 4096


def _user_base(root: Path) -> int | None:
    """USER_BASE from user/user.ld -- the same derivation scripts/build.sh:396
    uses, so the sensor and the gate can never disagree about the ceiling."""
    ld = root / "user" / "user.ld"
    try:
        text = ld.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return None
    m = re.search(r"^\s*\.\s*=\s*(0x[0-9A-Fa-f]+)", text, re.MULTILINE)
    return int(m.group(1), 16) if m else None


def _bss_end(root: Path) -> int | None:
    """Highest `b`/`B` (BSS) symbol address in build/kernel.map -- the same
    symbol class scripts/build.sh:402 selects."""
    mp = root / "build" / "kernel.map"
    try:
        text = mp.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return None
    best = None
    for line in text.splitlines():
        m = re.match(r"^([0-9A-Fa-f]+)\s+[bB]\s", line)
        if not m:
            continue
        v = int(m.group(1), 16)
        if best is None or v > best:
            best = v
    return best


def headroom(root: Path) -> dict:
    ub, be = _user_base(root), _bss_end(root)
    if ub is None or be is None:
        # No map yet (clean tree) is NOT a finding -- the sensor is advisory
        # and must never turn a missing artifact into a scary verdict.
        return {"ok": True, "known": False,
                "reason": "no build/kernel.map or no USER_BASE yet"}
    gap = ub - be
    return {
        "ok": True,
        "known": True,
        "user_base": hex(ub),
        "bss_end": hex(be),
        "headroom_bytes": gap,
        "tight": gap < TIGHT_BYTES,
        "collided": gap <= 0,
        "advice": (
            "COLLIDED -- the build will refuse; raise USER_BASE in user/user.ld "
            "or cut kernel static allocations before implementing"
            if gap <= 0 else
            f"TIGHT -- {gap} bytes (< one 4K page). A section adding kernel "
            ".text/.rodata should park its carriage EARLY; .rodata/.data/.bss "
            "each ALIGN(4K), so the next overshoot is a whole page, not a byte"
            if gap < TIGHT_BYTES else
            f"OK -- {gap} bytes of slack"
        ),
    }


def _selftest() -> int:
    import tempfile
    fails = []

    def check(name, cond):
        print(("OK   " if cond else "FAIL ") + name)
        if not cond:
            fails.append(name)

    with tempfile.TemporaryDirectory() as d:
        r = Path(d)
        (r / "user").mkdir()
        (r / "build").mkdir()
        (r / "user" / "user.ld").write_text("SECTIONS {\n  . = 0x800000;\n}\n")

        # roomy
        (r / "build" / "kernel.map").write_text("00000000007f0000 b some_bss\n")
        h = headroom(r)
        check("roomy: not tight, not collided",
              h["known"] and not h["tight"] and not h["collided"]
              and h["headroom_bytes"] == 0x10000)

        # tight (under one page)
        (r / "build" / "kernel.map").write_text("00000000007ffe00 B late_bss\n")
        h = headroom(r)
        check("tight: flagged under one 4K page", h["tight"] and not h["collided"])

        # REFUSAL DIRECTION: a real collision must still read as collided, so
        # the sensor can never soften what build.sh will refuse.
        (r / "build" / "kernel.map").write_text("0000000000800100 b over_bss\n")
        h = headroom(r)
        check("collision: reported as collided", h["collided"] and h["headroom_bytes"] < 0)

        # REFUSAL DIRECTION: highest symbol wins, not the last line read.
        (r / "build" / "kernel.map").write_text(
            "00000000007ffe00 b late_bss\n00000000007f0000 b early_bss\n")
        check("highest BSS symbol wins", headroom(r)["bss_end"] == "0x7ffe00")

        # non-BSS symbol classes must be ignored
        (r / "build" / "kernel.map").write_text(
            "00000000007f0000 b some_bss\n0000000000900000 T a_text_sym\n")
        check("non-BSS symbols ignored", headroom(r)["bss_end"] == "0x7f0000")

        # missing artifacts are not a finding
        (r / "build" / "kernel.map").unlink()
        h = headroom(r)
        check("missing map: advisory, not a finding", h["ok"] and not h["known"])

    print("bss-headroom selftest " + ("OK" if not fails else f"FAILED: {fails}"))
    return 1 if fails else 0


def main(argv) -> int:
    if "--selftest" in argv:
        return _selftest()
    root = Path(__file__).resolve().parents[2]
    print(json.dumps(headroom(root), indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
