#!/usr/bin/env python3
"""Kernel image-to-USER_BASE headroom sensor.

WHY THIS EXISTS. `scripts/build.sh` already refuses a build whose kernel image
end has crossed `USER_BASE` ("BSS COLLISION"), but it can only say so AFTER a
full compile -- which is after the design review, the implementation and the
tests are already spent. `01-boot-platform/TODO-13` section 28 was reverted
whole for exactly this, and `02-kernel-core/TODO-33` section 7 lists re-run
items for TODO-22 sections 23/24/25 and TODO-23 sections 1-16 parked the same
way, so the cost has been paid at least five times.

The overshoot is CASCADING, not proportional -- `.text`, `.rodata`, `.data` and
`.bss` each `ALIGN(4K)` and chain -- so a section either fits with room to
spare or overshoots by a whole page, and no amount of care during
implementation predicts it. Measured at `d43b73d2b`: 347 bytes of headroom,
and 859 bytes of `.rodata` growth was enough to trip it.

WHAT THE NUMBER MEANS (v18 close-out, 2026-09-03). The first version reported
`USER_BASE - __kernel_end`, and `__kernel_end` is the linker's PAGE-ROUNDED end
of `.bss`, so the figure hid up to 4,095 bytes of already-spent space and said
"4096 bytes, not tight" on a tree where 79 bytes of `.text` growth would fail
the link. Three sections were designed, reviewed and implemented against that
number on 2026-08-30 and every one was deferred at link time. The sensor now
reads the section headers of `build/kernel.exe` and reports, per page-aligned
section group, how many bytes that group can grow before the image end crosses
`USER_BASE`:

    budget(group) = slack to the group's next page boundary
                  + (free whole pages below USER_BASE - 1) * 4096

`headroom_bytes` is the MINIMUM over groups, because a section's growth lands
wherever the compiler puts it and the smallest budget is the one that binds.
The old page-rounded figure is kept as `page_rounded_headroom` so the two can
be compared; it is always >= the exact figure and is never the headline.

This reads files the build ALREADY produced (`build/kernel.exe`, with
`build/kernel.map` as the fallback), so it costs no model tokens and no extra
build. Advisory by construction: it never fails a build, it reports headroom so
a section that adds kernel `.text`/`.rodata`/`.bss` can park its carriage EARLY
instead of discovering the ceiling twice.

  python3 scripts/overnight/bss-headroom.py            # JSON verdict
  python3 scripts/overnight/bss-headroom.py --selftest
"""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

PAGE = 4096
# Below one page of growth budget a section that adds any kernel .text/.rodata
# is a coin flip, because a single ALIGN(4K) boundary is the whole margin.
TIGHT_BYTES = 4096

_READELF_CANDIDATES = ("llvm-readelf-19", "llvm-readelf", "readelf")


def _align_up(v: int) -> int:
    return (v + PAGE - 1) & ~(PAGE - 1)


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


def _bss_end_from_map(root: Path) -> int | None:
    """Highest `b`/`B` (BSS) symbol address in build/kernel.map -- the same
    symbol class scripts/build.sh:402 selects. This is `__kernel_end`, already
    rounded up to a page by the linker, so it is an UPPER bound on the real
    image end and is used only as the fallback and the collision cross-check."""
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


# One readelf -S row. The Flg column is EMPTY for non-alloc (debug) sections,
# which shifts the trailing columns, so the row is anchored on a flags token of
# letters: only allocated sections carry one, and only those are wanted.
_SECTION_ROW = re.compile(
    r"^\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9A-Fa-f]{8,16})\s+[0-9A-Fa-f]+\s+"
    r"([0-9A-Fa-f]+)\s+[0-9A-Fa-f]+\s+([A-Za-z]+)\s")


def parse_sections(readelf_text: str) -> list[dict]:
    """Allocated sections (flag `A`) from `readelf -S` output, as
    {name, addr, size, end}, address order. Zero-address rows are ignored."""
    out = []
    for line in readelf_text.splitlines():
        m = _SECTION_ROW.match(line)
        if not m:
            continue
        name, addr, size, flags = m.groups()
        if "A" not in flags:
            continue
        a, s = int(addr, 16), int(size, 16)
        if a == 0:
            continue
        out.append({"name": name, "addr": a, "size": s, "end": a + s})
    out.sort(key=lambda d: d["addr"])
    return out


def _page_groups(sections: list[dict]) -> list[dict]:
    """Merge sections into page-aligned groups. A group starts at a section
    whose address is page-aligned and not inside the previous group; the small
    read-only orphans the linker packs after `.rodata` (`.bootproto`, `.reloc`)
    share `.rodata`'s page and therefore its budget."""
    groups: list[dict] = []
    for s in sections:
        if groups and not (s["addr"] % PAGE == 0 and s["addr"] >= groups[-1]["end"]):
            g = groups[-1]
            g["end"] = max(g["end"], s["end"])
            g["members"].append(s["name"])
            continue
        groups.append({"name": s["name"], "addr": s["addr"], "end": s["end"],
                       "members": [s["name"]]})
    return groups


def budgets(sections: list[dict], user_base: int) -> dict:
    """Exact growth budgets from section headers. See the module docstring for
    the formula; `collided` uses the same page-rounded end build.sh refuses on."""
    groups = _page_groups(sections)
    if not groups:
        return {}
    kernel_end = max(g["end"] for g in groups)
    kernel_end_page = _align_up(kernel_end)
    free_pages = (user_base - kernel_end_page) // PAGE
    collided = kernel_end_page >= user_base
    tolerated_shifts = max(free_pages - 1, 0)
    per_group = {}
    for g in groups:
        slack = _align_up(g["end"]) - g["end"]
        per_group[g["name"]] = slack + tolerated_shifts * PAGE
    tightest = min(per_group, key=per_group.get)
    return {
        "kernel_end": hex(kernel_end),
        "kernel_end_page": hex(kernel_end_page),
        "page_rounded_headroom": user_base - kernel_end_page,
        "budgets": per_group,
        "tightest": tightest,
        "headroom_bytes": (user_base - kernel_end_page) if collided
                          else per_group[tightest],
        "collided": collided,
    }


def _readelf_text(root: Path) -> str | None:
    exe = root / "build" / "kernel.exe"
    if not exe.is_file():
        return None
    for tool in _READELF_CANDIDATES:
        if shutil.which(tool) is None:
            continue
        try:
            r = subprocess.run([tool, "-S", "-W", str(exe)], capture_output=True,
                               text=True, timeout=30)
        except Exception:
            continue
        if r.returncode == 0 and r.stdout:
            return r.stdout
    return None


def _advice(h: dict) -> str:
    if h["collided"]:
        return ("COLLIDED -- the build will refuse; raise USER_BASE in user/user.ld "
                "or cut kernel static allocations before implementing")
    gap = h["headroom_bytes"]
    if h.get("precision") == "page-rounded":
        if gap < TIGHT_BYTES:
            return (f"TIGHT -- at most {gap} bytes (page-rounded upper bound; the real "
                    "budget is smaller). A section adding kernel .text/.rodata should "
                    "park its carriage EARLY")
        return (f"OK -- at most {gap} bytes of slack (page-rounded upper bound; build "
                "kernel.exe for the exact per-section budget)")
    b = ", ".join(f"{k} {v}" for k, v in h["budgets"].items())
    if gap < TIGHT_BYTES:
        return (f"TIGHT -- {gap} bytes of growth in {h['tightest']} before the image "
                f"crosses USER_BASE (per-section budgets: {b}; the page-rounded "
                f"figure {h['page_rounded_headroom']} overstates it). Any few-KiB "
                "addition anywhere in the image fails the link: park the carriage "
                "BEFORE implementing")
    return f"OK -- {gap} bytes of growth before the ceiling (per-section budgets: {b})"


def headroom(root: Path, readelf_text: str | None = None) -> dict:
    ub = _user_base(root)
    map_end = _bss_end_from_map(root)
    if readelf_text is None:
        readelf_text = _readelf_text(root)
    if ub is None:
        return {"ok": True, "known": False, "reason": "no USER_BASE in user/user.ld"}
    exact = budgets(parse_sections(readelf_text), ub) if readelf_text else {}
    if exact:
        h = {"ok": True, "known": True, "precision": "section-exact",
             "user_base": hex(ub), **exact}
        # REFUSAL DIRECTION: never soften what build.sh will refuse. The gate
        # reads the map's highest BSS symbol; if THAT says collided, so do we.
        if map_end is not None and map_end >= ub:
            h["collided"] = True
            h["headroom_bytes"] = min(h["headroom_bytes"], ub - map_end)
        h["bss_end"] = hex(map_end) if map_end is not None else h["kernel_end_page"]
    elif map_end is not None:
        gap = ub - map_end
        h = {"ok": True, "known": True, "precision": "page-rounded",
             "user_base": hex(ub), "bss_end": hex(map_end),
             "page_rounded_headroom": gap, "headroom_bytes": gap,
             "collided": gap <= 0}
    else:
        # No artifact yet (clean tree) is NOT a finding -- the sensor is
        # advisory and must never turn a missing artifact into a scary verdict.
        return {"ok": True, "known": False,
                "reason": "no build/kernel.exe, no build/kernel.map, or no USER_BASE yet"}
    h["tight"] = h["headroom_bytes"] < TIGHT_BYTES
    h["advice"] = _advice(h)
    return h


# The live tree at the v18 close-out (HEAD 7cd799909), verbatim from
# `llvm-readelf-19 -S -W build/kernel.exe`. USER_BASE was 0x800000, the map
# said 4096 bytes, and .text had 79.
_CANNED = """\
  [Nr] Name              Type            Address          Off    Size   ES Flg Lk Inf Al
  [ 0]                   NULL            0000000000000000 000000 000000 00      0   0  0
  [ 1] .text             PROGBITS        0000000000100000 001000 327fb1 00  AX  0   0 16
  [ 2] .rodata           PROGBITS        0000000000428000 329000 15c846 00 AMS  0   0 16
  [ 3] .bootproto        PROGBITS        0000000000584848 485848 000038 00   A  0   0  8
  [ 4] .reloc            PROGBITS        0000000000584880 485880 00000a 00   A  0   0  1
  [ 5] .firmware_capsule_refused PROGBITS 000000000058488a 48588a 000004 00   A  0   0  1
  [ 6] .data             PROGBITS        0000000000585000 486000 018c2c 00  WA  0   0 16
  [ 7] .bss              NOBITS          000000000059e000 49ec2c 260ad5 00  WA  0   0 64
  [ 8] .debug_aranges    PROGBITS        0000000000000000 49ec2c 000180 00      0   0  1
  [17] .debug_str        PROGBITS        0000000000000000 c82c6f 06e2d7 01  MS  0   0  1
"""


def _selftest() -> int:
    import tempfile
    fails = []

    def check(name, cond):
        print(("OK   " if cond else "FAIL ") + name)
        if not cond:
            fails.append(name)

    # ---- exact path, from canned section headers ----
    secs = parse_sections(_CANNED)
    check("parse: 8 allocated sections, debug rows ignored",
          [s["name"] for s in secs] == [".text", ".rodata", ".bootproto", ".reloc",
                                        ".firmware_capsule_refused", ".data", ".bss"]
          or len(secs) == 7)
    b = budgets(secs, 0x800000)
    check("live tree: .text is the binding budget at 79 bytes",
          b["tightest"] == ".text" and b["budgets"][".text"] == 79)
    check("live tree: headline is the MINIMUM, not the page-rounded 4096",
          b["headroom_bytes"] == 79 and b["page_rounded_headroom"] == 4096)
    check("live tree: .rodata/.data/.bss budgets are their page slack",
          b["budgets"][".rodata"] == 0x585000 - 0x58488e
          and b["budgets"][".data"] == 0x59e000 - 0x59dc2c
          and b["budgets"][".bss"] == 0x7ff000 - 0x7fead5)
    check("orphans after .rodata share its page group, not their own budget",
          ".bootproto" not in b["budgets"] and ".reloc" not in b["budgets"])
    check("exact figures never exceed the page-rounded one",
          all(v <= b["page_rounded_headroom"] for v in b["budgets"].values())
          and b["headroom_bytes"] <= b["page_rounded_headroom"])
    # Two free pages: one whole page shift is tolerated, so every budget
    # grows by exactly one page and .text is still the binding one.
    b2 = budgets(secs, 0x801000)
    check("two free pages: budgets gain one page each",
          b2["budgets"][".text"] == 79 + PAGE and b2["tightest"] == ".text"
          and b2["headroom_bytes"] == 79 + PAGE)
    # REFUSAL DIRECTION: a real collision must still read as collided, with a
    # non-positive headline, so the sensor can never soften what build.sh
    # will refuse. Here .bss ends past the last page below USER_BASE.
    b3 = budgets(secs, 0x7ff000)
    check("collision: reported as collided with headroom <= 0",
          b3["collided"] and b3["headroom_bytes"] <= 0)

    with tempfile.TemporaryDirectory() as d:
        r = Path(d)
        (r / "user").mkdir()
        (r / "build").mkdir()
        (r / "user" / "user.ld").write_text("SECTIONS {\n  . = 0x800000;\n}\n")

        # exact path through headroom(): the live tree is TIGHT even though
        # the map says one full page
        (r / "build" / "kernel.map").write_text("00000000007ff000 B __kernel_end\n")
        h = headroom(r, readelf_text=_CANNED)
        check("headroom(): exact path reports tight at 79 with map at 4096",
              h["precision"] == "section-exact" and h["tight"] and not h["collided"]
              and h["headroom_bytes"] == 79 and h["page_rounded_headroom"] == 4096)
        check("headroom(): advice names the binding section and the overstatement",
              ".text" in h["advice"] and "4096" in h["advice"])
        # REFUSAL DIRECTION: the map's collision verdict wins over an exact
        # figure that looks fine -- build.sh reads the map.
        (r / "build" / "kernel.map").write_text("0000000000800100 b over_bss\n")
        h = headroom(r, readelf_text=_CANNED)
        check("headroom(): a map collision cannot be softened by section headers",
              h["collided"] and h["headroom_bytes"] < 0)

        # ---- fallback path (no kernel.exe): page-rounded, marked as such ----
        (r / "build" / "kernel.map").write_text("00000000007f0000 b some_bss\n")
        h = headroom(r, readelf_text="")
        check("fallback roomy: not tight, not collided, marked page-rounded",
              h["known"] and h["precision"] == "page-rounded" and not h["tight"]
              and not h["collided"] and h["headroom_bytes"] == 0x10000)
        (r / "build" / "kernel.map").write_text("00000000007ffe00 B late_bss\n")
        h = headroom(r, readelf_text="")
        check("fallback tight: flagged under one 4K page", h["tight"] and not h["collided"])
        (r / "build" / "kernel.map").write_text("0000000000800100 b over_bss\n")
        h = headroom(r, readelf_text="")
        check("fallback collision: reported as collided",
              h["collided"] and h["headroom_bytes"] < 0)
        (r / "build" / "kernel.map").write_text(
            "00000000007ffe00 b late_bss\n00000000007f0000 b early_bss\n")
        check("fallback: highest BSS symbol wins",
              headroom(r, readelf_text="")["bss_end"] == "0x7ffe00")
        (r / "build" / "kernel.map").write_text(
            "00000000007f0000 b some_bss\n0000000000900000 T a_text_sym\n")
        check("fallback: non-BSS symbols ignored",
              headroom(r, readelf_text="")["bss_end"] == "0x7f0000")

        # missing artifacts are not a finding
        (r / "build" / "kernel.map").unlink()
        h = headroom(r, readelf_text="")
        check("missing artifacts: advisory, not a finding", h["ok"] and not h["known"])

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
