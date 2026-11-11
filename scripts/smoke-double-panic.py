#!/usr/bin/env python3
"""Verdict for the double-panic smoke legs (SMOKE_CRASH_TEST=double-*).

Two CPUs panic in one boot (crash_test=4, 5 or 6: an NMI makes an AP raise
KeBugCheckExFrame(0xE2) while the BSP panics with code 0xDEAD), then the guest
is warm-reset and the next boot reports the cross-boot evidence record. This
reads the ANSI-stripped serial log of both boots and fails unless every
acknowledgment is there and agrees:

  * the fixture named the AP it sent the NMI to, and no ordering timeout;
  * exactly two panic reports, each a whole block ending in its own
    "  on CPU apic=0x.." line, from two different CPUs, one of them that AP;
  * exactly one whole "secondary panic ... parked" line, naming one of them
    as parked and the other (by logical id: the AP's from the fixture line,
    the BSP's 0) as the owner;
  * exactly one stop screen, whose stop code is the WINNER's bugcheck;
  * the crash dump landed;
  * the next boot found the record, and its STOP is the winner's code.

The mode fixes which CPU must win: "ap" (crash_test=5) the AP, "split"
(crash_test=6, the AP owns the evidence page first and is held before its
owner claim) the BSP, "race" (crash_test=4) either, as long as every line
agrees on which.

    python3 scripts/smoke-double-panic.py check <stripped-serial-log> <race|ap|split>

Prints "STOP_NAME=<name>" (the stop code the screen must show) and a summary
on success; prints the reason and exits 1 otherwise.
"""
from __future__ import annotations

import pathlib
import re
import sys

MODES = {"race": 4, "ap": 5, "split": 6}
LOADER_ENTRY = "efi_main entered"
AP_STOP = ("MANUALLY_INITIATED_CRASH", 0xE2)
BSP_STOP = ("KERNEL_PANIC", 0xDEAD)

ORDER_RE = re.compile(r"crash_test=(\d+) -- NMI to CPU (\d+) \(apic (\d+)\), then a BSOD here")
ON_CPU_RE = re.compile(r"^  on CPU apic=0x([0-9A-F]+)$")
PARK_RE = re.compile(r"^\[PANIC\] secondary panic on CPU apic=0x([0-9A-F]+) parked; "
                     r"logical CPU 0x([0-9A-F]+) owns the crash$")
STOP_RE = re.compile(r"^\s*Stop code:\s+(\S+)\s*$")
RECORD_RE = re.compile(r"Previous crash evidence found \(STOP 0x([0-9a-f]+) rip=0x[0-9a-f]+\)")
DUMP = r"Crash dump saved to C:\Impossible\System\crashdump.log"
FORBIDDEN = ("never reached the ordering point", "needs a second online CPU")
# A report's header, then at most the "  at <file>" line, then its CPU line.
REPORT_SPAN = 3


class VerdictError(Exception):
    pass


def split_boots(text: str) -> list[list[str]]:
    """The log's lines, one list per loader entry (lines before the first dropped)."""
    boots: list[list[str]] = []
    for line in text.splitlines():
        if LOADER_ENTRY in line:
            boots.append([])
        if boots:
            boots[-1].append(line)
    return boots


def reports(lines: list[str]) -> list[int]:
    """APIC id of every panic report, in order. A report whose CPU line is
    missing or not whole (another CPU's bytes braided into it) is an error."""
    found = []
    for i, line in enumerate(lines):
        if not line.startswith("[PANIC] ") or PARK_RE.match(line):
            continue
        for follow in lines[i + 1:i + 1 + REPORT_SPAN]:
            m = ON_CPU_RE.match(follow)
            if m:
                found.append(int(m.group(1), 16))
                break
        else:
            raise VerdictError("panic report %r has no whole CPU line after it" % line[:60])
    return found


def check(text: str, mode: str) -> tuple[str, str]:
    if mode not in MODES:
        raise VerdictError("mode must be one of %s" % ", ".join(MODES))
    boots = split_boots(text)
    if len(boots) < 2:
        raise VerdictError("expected the crash boot and the boot after the reset, saw %d" % len(boots))
    first, second = boots[0], boots[1]
    body = "\n".join(first)

    for bad in FORBIDDEN:
        if bad in body:
            raise VerdictError("fixture: %s" % bad)
    orders = [m for m in (ORDER_RE.search(x) for x in first) if m]
    if len(orders) != 1:
        raise VerdictError("expected one fixture NMI line, saw %d" % len(orders))
    if int(orders[0].group(1)) != MODES[mode]:
        raise VerdictError("fixture ran crash_test=%s, mode %s needs %d"
                           % (orders[0].group(1), mode, MODES[mode]))
    ap = int(orders[0].group(3))
    ap_logical = int(orders[0].group(2))

    cpus = reports(first)
    if len(cpus) != 2 or len(set(cpus)) != 2:
        raise VerdictError("expected one panic report from each of two CPUs, saw apic %s"
                           % (cpus or "none"))
    if ap not in cpus:
        raise VerdictError("no panic report from the NMI'd AP (apic %d); reports from %s" % (ap, cpus))

    parks = [m for m in (PARK_RE.match(x) for x in first) if m]
    if len(parks) != 1:
        raise VerdictError("expected one whole secondary-panic park line, saw %d" % len(parks))
    loser = int(parks[0].group(1), 16)
    if loser not in cpus:
        raise VerdictError("the parked CPU (apic %d) never reported a panic" % loser)
    winner = cpus[0] if cpus[1] == loser else cpus[1]
    # The park line names the owner by LOGICAL id; the fixture's order line maps
    # the AP's, and the BSP is logical CPU 0. It must name the winner.
    owner_logical = int(parks[0].group(2), 16)
    expected_owner = ap_logical if winner == ap else 0
    if owner_logical != expected_owner:
        raise VerdictError("the park line says logical CPU %d owns the crash, the winner (apic %d) is "
                           "logical CPU %d" % (owner_logical, winner, expected_owner))
    if mode == "ap" and winner != ap:
        raise VerdictError("mode ap: the AP (apic %d) must win, apic %d did" % (ap, winner))
    if mode == "split" and loser != ap:
        raise VerdictError("mode split: the AP (apic %d) must park, apic %d did" % (ap, loser))
    name, code = AP_STOP if winner == ap else BSP_STOP

    stops = [m.group(1) for m in (STOP_RE.match(x) for x in first) if m]
    if stops != [name]:
        raise VerdictError("expected one stop screen showing %s, saw %s" % (name, stops or "none"))
    if DUMP not in body:
        raise VerdictError("the crash dump never landed")

    records = [m for m in (RECORD_RE.search(x) for x in second) if m]
    if len(records) != 1:
        raise VerdictError("the boot after the reset reported %d crash record(s), expected 1"
                           % len(records))
    got = int(records[0].group(1), 16)
    if got != code:
        raise VerdictError("the record names STOP 0x%x, the winner (apic %d) was 0x%x"
                           % (got, winner, code))
    return name, ("winner apic %d (%s), loser apic %d parked, record STOP 0x%x on the next boot"
                  % (winner, name, loser, got))


def main(argv: list[str]) -> int:
    if len(argv) != 3 or argv[0] != "check":
        print(__doc__)
        return 2
    try:
        name, summary = check(pathlib.Path(argv[1]).read_text(encoding="utf-8", errors="replace"),
                              argv[2])
    except (VerdictError, OSError) as exc:
        print(exc)
        return 1
    print("STOP_NAME=%s" % name)
    print(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
