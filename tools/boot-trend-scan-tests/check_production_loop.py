#!/usr/bin/env python3
"""Bind the scan-policy fixtures to the loop that actually ships.

test_scan.c exercises `boot_trend_scan_action()` through a simulator that
reproduces the publisher's loop. That simulator is a COPY, so it stays green
if the production loop drifts away from the helper -- restoring
`kept < BOOT_TREND_RING_DEPTH` to the loop condition reinstates the exact
silent-truncation defect the fixtures exist to catch, and every one of them
still passes. This closes that gap from the source side: no kernel image cost,
which the 15 bytes of `.text` headroom rules out anything else.

Five properties of the existing-entry walk in src/kernel/main/boot_trend.c:

  1. its `for` header advances the cursor ONLY -- no `kept` or ring-depth term,
     because a ring-full exit is the defect;
  2. every reached node is routed through `boot_trend_scan_action(`;
  3. the body acts on both STOP and COUNT_ONLY;
  4. the bound's only externally observable output -- the oversize WARN -- is
     still emitted after the walk. The fixtures compute their own `warned` from
     the simulator, so deleting that klog leaves every one of them green while
     restoring the silent-truncation outcome the control case exists to reject;
  5. `kept` still enters the walk at 1, for the current boot emitted ahead of
     it. The whole COUNT_ONLY rationale is stated in terms of that initialiser
     (a ring of 16 fills after 15 EXISTING entries); start it at 0, or move the
     current-entry emission after the loop, and the output grows to
     RING_DEPTH + 1 entries with every fixture and property above still green.

The self-test at the bottom is the point of the file: it re-runs the same
checker over a mutated copy carrying the ring-full exit and requires a FAILURE.
Without it this would pass just as happily against a checker that matched
nothing at all.

  python3 tools/boot-trend-scan-tests/check_production_loop.py

Exit: 0 all properties hold and the control fired, 1 a property failed,
2 the loop could not be located (a refactor, not a regression -- fix the
locator rather than deleting the gate).
"""
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
SRC = REPO / "src/kernel/main/boot_trend.c"
ANCHOR = "json_array_first(boots_arr)"


def find_loop(text):
    """Return (for_header, body) for the walk seeded by json_array_first."""
    at = text.find(ANCHOR)
    if at < 0:
        return None
    m = re.compile(r"for\s*\((.*?)\)\s*\{", re.S).search(text, at)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(text) and depth:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    if depth:
        return None
    return m.group(1), text[m.end():i - 1]


def enclosing_function(text):
    """Body of boot_trend_publish_json, brace-matched from its opening `{`."""
    m = re.search(r"void\s+boot_trend_publish_json\s*\([^)]*\)\s*\{", text)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(text) and depth:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return None if depth else text[m.end():i - 1]


def check(text, label, out):
    found = find_loop(text)
    if found is None:
        out.append((False, f"{label}: could not locate the walk after "
                           f"'{ANCHOR}'", True))
        return
    header, body = found
    out.append((("kept" not in header
                 and "BOOT_TREND_RING_DEPTH" not in header),
                f"{label}: loop header advances the cursor only "
                f"(no ring-full exit) -- header was: for ({header.strip()})",
                False))
    out.append(("boot_trend_scan_action(" in body,
                f"{label}: every reached node goes through "
                f"boot_trend_scan_action()", False))
    out.append(("BOOT_TREND_SCAN_STOP" in body
                and "BOOT_TREND_SCAN_COUNT_ONLY" in body,
                f"{label}: body acts on both STOP and COUNT_ONLY", False))

    # The walk's own text ends at its closing brace, so the WARN and the
    # initialiser are searched in the enclosing function instead.
    fn = enclosing_function(text)
    if fn is None:
        out.append((False, f"{label}: could not locate the enclosing publisher",
                    True))
        return
    out.append((bool(re.search(r"if\s*\(\s*seen\s*>\s*BOOT_TREND_MAX_SCAN\s*\)\s*"
                               r"\n?\s*klog\s*\(\s*LOG_WARN", fn)),
                f"{label}: the oversize bound still WARNs after the walk", False))
    out.append((bool(re.search(r"\buint32_t\s+kept\s*=\s*1\s*;", fn)),
                f"{label}: kept enters the walk at 1 (current boot already "
                f"emitted)", False))


def main():
    if not SRC.exists():
        print(f"  [FAIL] {SRC} not found", file=sys.stderr)
        return 2
    text = SRC.read_text()

    results = []
    check(text, "production", results)
    if any(fatal for _, _, fatal in results):
        for ok, what, _ in results:
            print(f"  [{'PASS' if ok else 'FAIL'}] {what}")
        return 2

    # CONTROL: reinstate the ring-full exit and require the checker to refuse.
    found = find_loop(text)
    header = found[0]
    mutated = text.replace(f"for ({header})",
                           f"for ({header.rstrip()} && kept < BOOT_TREND_RING_DEPTH)",
                           1)
    if mutated == text:
        print("  [FAIL] CONTROL: could not mutate the loop header")
        return 2
    mutations = [
        ("a reinstated ring-full exit", mutated),
        ("a deleted oversize WARN",
         re.sub(r"if \(seen > BOOT_TREND_MAX_SCAN\)\n\s*klog\(LOG_WARN,"
                r"(?:.|\n)*?\);\n", "", text, count=1)),
        ("kept starting at 0",
         text.replace("uint32_t kept = 1;", "uint32_t kept = 0;", 1)),
    ]
    for what, mut in mutations:
        if mut == text:
            results.append((False, f"CONTROL: could not apply the mutation "
                                   f"for {what}", False))
            continue
        control = []
        check(mut, "CONTROL", control)
        results.append((any(not ok for ok, _, _ in control),
                        f"CONTROL: {what} is REFUSED by this gate", False))

    failures = 0
    for ok, what, _ in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {what}")
        failures += 0 if ok else 1
    print(f"{'FAIL' if failures else 'PASS'}: {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
