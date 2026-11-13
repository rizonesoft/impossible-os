#!/usr/bin/env python3
"""Fixtures for scripts/lib/optin_path_check.py (the identity and timeout proofs).

Each mode must accept a well-formed run of its fixture and refuse the near
misses the proof exists to catch: for identity, a victim that EXECUTED (its
marker on serial), a victim refused for another reason, a duplicated record,
and another run's artifacts; for timeout, a reap that left SIGKILL's -9, a
hang that never reached ring 3, a timeout with a different limit, and counts
that do not reconcile.

    python3 scripts/tests/test_optin_path_check.py
"""
import importlib.util
import json
import os
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "optin_path_check", HERE.parent / "lib" / "optin_path_check.py")
opc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(opc)

RUN = "20261113T000000Z-1-abcdef01"
OTHER = "20261113T000001Z-2-12345678"


def doc(cases, run_id=RUN, total=None, aborted=False, not_run=0):
    return {"run_identity": {"run_id": run_id},
            "summary": {"aborted": aborted, "not_run": not_run,
                        "total": len(cases) if total is None else total},
            "testcases": cases}


def xml(cases, run_id=RUN):
    body = []
    for name, kind, msg in cases:
        inner = f'<{kind} message="{msg}"/>' if kind else ""
        body.append(f'  <testcase name="{name}" classname="correctness">{inner}</testcase>')
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<testsuite name="impossible-os-usermode">\n'
            f'  <properties><property name="run_id" value="{run_id}"/></properties>\n'
            + "\n".join(body) + "\n</testsuite>\n")


SMOKE = {"name": "test_smoke_boot.exe", "status": "PASS"}
SMOKE_X = ("test_smoke_boot.exe", None, None)


def identity(victim_status="ERROR", victim_reason="planned binary replaced",
             extra_victim=False, run_id=RUN, xml_run_id=RUN, slice_text="",
             xml_kind="error", captured=None):
    cases = [SMOKE, {"name": "test_optin_swap.exe", "status": "PASS"},
             {"name": "test_optin_victim.exe", "status": victim_status,
              "reason": victim_reason, "captured_output": captured}]
    xcases = [SMOKE_X, ("test_optin_swap.exe", None, None),
              ("test_optin_victim.exe", xml_kind, victim_reason)]
    if extra_victim:
        cases.append(dict(cases[-1]))
        xcases.append(xcases[-1])
    return doc(cases, run_id), xml(xcases, xml_run_id), slice_text, 3 + extra_victim


REAP_OK = ("UTEST-1: test_optin_hang.exe: reaped with exit status -1003 "
           "(TASK_EXIT_UTEST_TIMEOUT)\n")
REAP_KILL = ("UTEST-1: test_optin_hang.exe: reaped with exit status -9 "
             "(not_a_reserved_reason)\n")


def timeout(reason="timeout after 2000ms", captured="[OPTIN-HANG] in ring 3, yielding forever\n",
            slice_text=REAP_OK, total=None):
    cases = [SMOKE, {"name": "test_optin_hang.exe", "status": "FAIL", "reason": reason,
                     "captured_output": captured}]
    xcases = [SMOKE_X, ("test_optin_hang.exe", "failure", reason)]
    return doc(cases, RUN, total), xml(xcases), slice_text, 2


def verdict(mode, artifacts, ms=2000):
    d, x, sl, expected = artifacts
    with tempfile.TemporaryDirectory() as tmp:
        jp, xp, sp = (os.path.join(tmp, n) for n in ("r.json", "r.xml", "runslice"))
        with open(jp, "w") as fh:
            json.dump(d, fh)
        with open(xp, "w") as fh:
            fh.write(x)
        with open(sp, "w") as fh:
            fh.write(sl)
        argv = ["optin_path_check.py", mode, jp, xp, sp, RUN, str(expected)]
        if mode == "timeout":
            argv.append(str(ms))
        saved = sys.stderr
        sys.stderr = open(os.devnull, "w")
        try:
            return opc.main(argv)
        finally:
            sys.stderr.close()
            sys.stderr = saved


CASES = [
    ("identity: a refused, never-run victim passes", "identity", identity(), 0),
    ("identity: a victim that executed fails", "identity",
     identity(slice_text="[PASS] [OPTIN-VICTIM] replaced binary executed\n"), 1),
    ("identity: a refused record whose captured output holds the marker fails", "identity",
     identity(captured="[PASS] [OPTIN-VICTIM] replaced binary executed\n"), 1),
    ("identity: a victim that ran and passed fails", "identity",
     identity(victim_status="PASS", victim_reason=None), 1),
    ("identity: a victim refused for another reason fails", "identity",
     identity(victim_reason="exit=-1"), 1),
    ("identity: two victim records fail", "identity", identity(extra_victim=True), 1),
    ("identity: another run's JSON fails", "identity", identity(run_id=OTHER), 1),
    ("identity: another run's JUnit fails", "identity", identity(xml_run_id=OTHER), 1),
    ("identity: a JUnit failure in place of the error fails", "identity",
     identity(xml_kind="failure"), 1),
    ("timeout: a timeout reap passes", "timeout", timeout(), 0),
    ("timeout: a reap that left SIGKILL's -9 fails", "timeout", timeout(slice_text=REAP_KILL), 1),
    ("timeout: no reap line fails", "timeout", timeout(slice_text=""), 1),
    ("timeout: a hang that never reached ring 3 fails", "timeout", timeout(captured=""), 1),
    ("timeout: another limit fails", "timeout", timeout(reason="timeout after 10000ms"), 1),
    ("timeout: counts that do not reconcile fail", "timeout", timeout(total=5), 1),
]


def main():
    bad = 0
    for label, mode, artifacts, want in CASES:
        got = verdict(mode, artifacts)
        ok = got == want
        bad += not ok
        print(f"{'PASS' if ok else 'FAIL'} {label} (exit {got}, want {want})")
    print(f"{len(CASES) - bad}/{len(CASES)} passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
