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


REPLAY = "[CRASH-PREV] boot: crash_test=%d -- NMI to CPU 1 (apic 1), then a BSOD here"


def log(ct: int, first: list[str], stop: str, record: int | None, boots: int = 2,
        replay: int | None = -1) -> str:
    lines = ["[BOOT] efi_main entered", "[BOOT] Panic evidence: 0x00080000 pinned",
             "[  1.4] [cpu:0] [WARN] boot: crash_test=%d -- NMI to CPU 1 (apic 1), then a BSOD here" % ct]
    lines += first
    lines += ["    Stop code:  %s" % stop, DUMP]
    if boots >= 2:
        lines += ["[BOOT] efi_main entered"]
        if replay is not None:
            lines += ["[CRASH-PREV] === Recovered 2 entries from previous crash ===",
                      "[CRASH-PREV] boot: earlier", REPLAY % (ct if replay < 0 else replay)]
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
expect("a next boot that does not replay the log fails",
       log(4, bsp_wins, "KERNEL_PANIC", 0xDEAD, replay=None), "race", False)
expect("a replay of another crash's log fails",
       log(4, bsp_wins, "KERNEL_PANIC", 0xDEAD, replay=5), "race", False)
expect("a missing crash dump fails",
       log(4, bsp_wins, "KERNEL_PANIC", 0xDEAD).replace(DUMP + "\n", ""), "race", False)
expect("an ordering timeout fails",
       log(5, ap_wins + ["[ERROR] boot: crash_test=5: CPU 1 never reached the ordering point"],
           "MANUALLY_INITIATED_CRASH", 0xE2), "ap", False)
expect("an unknown mode fails", log(4, bsp_wins, "KERNEL_PANIC", 0xDEAD), "both", False)

LONE = "boot: crash_test=7 -- triggering deliberate BSOD whose disk dump is abandoned"
LONE_END = "[PANIC] crash dump abandoned: crash_test=7"
CRASH_BOOT = ["[BOOT] efi_main entered", "[  1.5] [cpu:0] [WARN] " + LONE, LONE_END]
RECORD = "[  0.000] [FAIL] panic: [PANIC] Previous crash evidence found (STOP 0xdead rip=0x0)"
NEXT_BOOT = ["[BOOT] efi_main entered", "[CRASH-PREV] " + LONE, RECORD] + CRASH_BOOT[1:]
BARE_BOOT = ["[BOOT] efi_main entered", RECORD] + CRASH_BOOT[1:]


def expect_lone(name: str, lines: list[str], ok: bool) -> None:
    global failures
    try:
        dp.check_replay("\n".join(lines) + "\n", LONE_END)
        got, why = True, ""
    except dp.VerdictError as exc:
        got, why = False, str(exc)
    if got != ok:
        failures += 1
    print("%s  %s%s" % ("PASS" if got == ok else "FAIL", name, ("  (" + why + ")") if why else ""))


expect_lone("a lone crash whose log the next boot replays passes", CRASH_BOOT + NEXT_BOOT, True)
expect_lone("a third boot changes nothing", CRASH_BOOT + NEXT_BOOT + NEXT_BOOT, True)
expect_lone("a replay that first comes in the third boot fails", CRASH_BOOT + BARE_BOOT + NEXT_BOOT, False)
expect_lone("no boot after the crash fails", CRASH_BOOT, False)
expect_lone("a replay without the record fails", CRASH_BOOT + [x for x in NEXT_BOOT if x != RECORD], False)
expect_lone("a crash that never reached its last line fails", CRASH_BOOT[:2] + NEXT_BOOT, False)
expect_lone("a crash that saved its dump fails", CRASH_BOOT + [DUMP] + NEXT_BOOT, False)


CUT_END = "[FIXTURE] crash_test=8: reset as the log is about to be saved"
CUT_REPORTS = ["[PANIC] MANUALLY_INITIATED_CRASH", "  on CPU apic=0x0000000000000001",
               "[PANIC] CRASH_TEST", "  on CPU apic=0x0000000000000000"]
CUT_ORDER = "[  1.5] [cpu:0] [WARN] boot: crash_test=8 -- NMI to CPU 1 (apic 1), then a BSOD here"
CUT_BOOT = ["[BOOT] efi_main entered", CUT_ORDER] + CUT_REPORTS + [CUT_END]


def expect_cut(name: str, lines: list[str], ok: bool) -> None:
    global failures
    try:
        dp.check_cut("\n".join(lines) + "\n", CUT_END)
        got, why = True, ""
    except dp.VerdictError as exc:
        got, why = False, str(exc)
    if got != ok:
        failures += 1
    print("%s  %s%s" % ("PASS" if got == ok else "FAIL", name, ("  (" + why + ")") if why else ""))


AFTER = ["[BOOT] efi_main entered", RECORD]
expect_cut("a cut crash whose record names the winner passes", CUT_BOOT + AFTER, True)
expect_cut("a record that names the loser fails",
           CUT_BOOT + [AFTER[0], RECORD.replace("0xdead", "0xe2")], False)
expect_cut("no record after a cut crash fails", CUT_BOOT + AFTER[:1] + CUT_BOOT + AFTER, False)
expect_cut("one report alone fails", CUT_BOOT[:4] + CUT_BOOT[6:] + AFTER, False)
expect_cut("a crash that was never cut fails", CUT_BOOT[:-1] + AFTER, False)


def expect_done(name: str, lines: list[str], tail: str, done: bool) -> None:
    global failures
    got = dp.next_boot_done("\n".join(lines + [tail]))
    if got != done:
        failures += 1
    print("%s  %s" % ("PASS" if got == done else "FAIL", name))


SECOND = CRASH_BOOT + ["[BOOT] efi_main entered"]
expect_done("half a record line is not a record yet", SECOND, RECORD[:-12], False)
expect_done("the whole record line is", SECOND, RECORD, True)
expect_done("the prompt without a record ends the wait", SECOND, "[  2.0] Boot complete", True)
expect_done("a third boot ends the wait", SECOND, "[BOOT] efi_main entered", True)
expect_done("a record or a prompt of the first boot does not", CRASH_BOOT + [RECORD], "Boot complete", False)

print("%d failure(s)" % failures)
sys.exit(1 if failures else 0)
