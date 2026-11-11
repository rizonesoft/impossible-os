#!/usr/bin/env python3
"""Fixtures for scripts/smoke-double-panic.py, the double-panic legs' verdict.

The passing logs are shaped like the real 2026-11-11 boots (crash_test=4/5/6
on 2 CPUs under KVM). Each failing one is a way the panic path has broken or
could break while a looser check would still pass: a report braided into
another CPU's output, a CPU that never acknowledged, the wrong winner, a stop
screen or record that names the other CPU's crash, a missing reset boot.

    python3 scripts/tests/test_smoke_double_panic.py
"""
from __future__ import annotations

import importlib.util
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("smoke_double_panic", HERE.parent / "smoke-double-panic.py")
dp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dp)

failures = 0

BSP_REPORT = ["", "", "[PANIC] CRASH_TEST: Deliberate panic racing a second CPU",
              "  at src/kernel/main/boot_desktop.c", "  on CPU apic=0x0"]
AP_REPORT = ["", "", "[PANIC] (unavailable: no guarded read in this context)",
             "  at (unavailable: no guarded read in this context)", "  on CPU apic=0x1",
             "  RIP=0x226E82 CS=0x8 ERR=0x0"]
DUMP = r"    Crash dump saved to C:\Impossible\System\crashdump.log"


def park(loser: int, owner: int) -> str:
    return ("[PANIC] secondary panic on CPU apic=0x%X parked; logical CPU 0x%X owns the crash"
            % (loser, owner))


def log(ct: int, first: list[str], stop: str, record: int | None, boots: int = 2) -> str:
    lines = ["[BOOT] efi_main entered", "[BOOT] Panic evidence: 0x00080000 pinned",
             "[  1.4] [cpu:0] [WARN] boot: crash_test=%d -- NMI to CPU 1 (apic 1), then a BSOD here" % ct]
    lines += first
    lines += ["    Stop code:  %s" % stop, DUMP]
    if boots >= 2:
        lines += ["[BOOT] efi_main entered"]
        if record is not None:
            lines += ["[  0.000] [FAIL] panic: [PANIC] Previous crash evidence found "
                      "(STOP 0x%x rip=0x0)" % record]
        lines += ["[  2.0] Boot complete"]
    return "\n".join(lines) + "\n"


def expect(name: str, text: str, mode: str, ok: bool, stop: str | None = None) -> None:
    global failures
    try:
        got_stop, _ = dp.check(text, mode)
        got, why = True, ""
    except dp.VerdictError as exc:
        got, got_stop, why = False, None, str(exc)
    good = got == ok and (stop is None or got_stop == stop)
    if not good:
        failures += 1
    print("%s  %s%s" % ("PASS" if good else "FAIL", name, ("  (" + why + ")") if why else ""))


bsp_wins = BSP_REPORT + AP_REPORT + [park(1, 0)]
ap_wins = AP_REPORT + BSP_REPORT + [park(0, 1)]

expect("race, the BSP wins, every line agrees",
       log(4, bsp_wins, "KERNEL_PANIC", 0xDEAD), "race", True, "KERNEL_PANIC")
expect("race, the AP wins, every line agrees",
       log(4, ap_wins, "MANUALLY_INITIATED_CRASH", 0xE2), "race", True, "MANUALLY_INITIATED_CRASH")
expect("ap mode with the AP winning passes",
       log(5, ap_wins, "MANUALLY_INITIATED_CRASH", 0xE2), "ap", True)
expect("split mode with the BSP winning over the AP's record passes",
       log(6, bsp_wins, "KERNEL_PANIC", 0xDEAD), "split", True)

expect("ap mode where the BSP won fails",
       log(5, bsp_wins, "KERNEL_PANIC", 0xDEAD), "ap", False)
expect("split mode where the AP won fails",
       log(6, ap_wins, "MANUALLY_INITIATED_CRASH", 0xE2), "split", False)
expect("a fixture value that does not match the mode fails",
       log(4, ap_wins, "MANUALLY_INITIATED_CRASH", 0xE2), "ap", False)

# The 2026-11-11 braid: the AP's park line cut into by the BSP's output.
braided = BSP_REPORT[:3] + ["[PANIC] secondary panic [on   C3P.U4 1a0p]i c[=cp0ux:10]  pa[r3k2emd"] + AP_REPORT
expect("a braided report fails", log(4, braided, "KERNEL_PANIC", 0xDEAD), "race", False)
cut = BSP_REPORT[:3] + ["  at src/kernel/main/boot_desktop.c", "  on CPU apic=0x[  1.6] [cpu:1] RING"] + AP_REPORT + [park(1, 0)]
expect("a CPU line with another CPU's bytes in it fails", log(4, cut, "KERNEL_PANIC", 0xDEAD), "race", False)

expect("only one CPU reported fails",
       log(4, BSP_REPORT + [park(1, 0)], "KERNEL_PANIC", 0xDEAD), "race", False)
expect("two reports from the same CPU fail",
       log(4, BSP_REPORT + BSP_REPORT + [park(0, 0)], "KERNEL_PANIC", 0xDEAD), "race", False)
expect("no park line fails",
       log(4, BSP_REPORT + AP_REPORT, "KERNEL_PANIC", 0xDEAD), "race", False)
expect("two park lines fail",
       log(4, bsp_wins + [park(0, 1)], "KERNEL_PANIC", 0xDEAD), "race", False)
expect("a park line naming an unrelated owner fails",
       log(5, AP_REPORT + BSP_REPORT + [park(0, 0xFF)], "MANUALLY_INITIATED_CRASH", 0xE2), "ap", False)
expect("a park line naming the parked CPU as owner fails",
       log(5, AP_REPORT + BSP_REPORT + [park(0, 0)], "MANUALLY_INITIATED_CRASH", 0xE2), "ap", False)
expect("a BSP win whose park line names the AP as owner fails",
       log(6, BSP_REPORT + AP_REPORT + [park(1, 1)], "KERNEL_PANIC", 0xDEAD), "split", False)
expect("a park line naming a CPU that never reported fails",
       log(4, BSP_REPORT + AP_REPORT + [park(3, 0)], "KERNEL_PANIC", 0xDEAD), "race", False)

expect("a stop screen naming the loser's crash fails",
       log(4, bsp_wins, "MANUALLY_INITIATED_CRASH", 0xDEAD), "race", False)
expect("the NMI vector name instead of the bugcheck fails",
       log(5, ap_wins, "NMI_INTERRUPT", 0xE2), "ap", False)
expect("a record naming the loser's crash fails",
       log(4, bsp_wins, "KERNEL_PANIC", 0xE2), "race", False)
expect("no record on the next boot fails",
       log(4, bsp_wins, "KERNEL_PANIC", None), "race", False)
expect("no boot after the reset fails",
       log(4, bsp_wins, "KERNEL_PANIC", 0xDEAD, boots=1), "race", False)
expect("a missing crash dump fails",
       log(4, bsp_wins, "KERNEL_PANIC", 0xDEAD).replace(DUMP + "\n", ""), "race", False)
expect("an ordering timeout fails",
       log(5, ap_wins + ["[ERROR] boot: crash_test=5: CPU 1 never reached the ordering point"],
           "MANUALLY_INITIATED_CRASH", 0xE2), "ap", False)
expect("an unknown mode fails", log(4, bsp_wins, "KERNEL_PANIC", 0xDEAD), "both", False)

print("%d failure(s)" % failures)
sys.exit(1 if failures else 0)
