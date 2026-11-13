"""Check the artifacts of one opt-in fixture run.

Usage: optin_path_check.py identity <json> <xml> <runslice> <run-id> <expected-total>
       optin_path_check.py timeout  <json> <xml> <runslice> <run-id> <expected-total> <timeout-ms>

identity (scripts/test-identity-path.sh): test_optin_swap.exe passed, and
test_optin_victim.exe has exactly one record, an ERROR with the reason
"planned binary replaced", in both the JSON and the JUnit file; the victim's
marker appears in neither its captured output nor serial; the run was not aborted, not_run is 0 and the
testcases reconcile with summary.total (<expected-total>: the default plan
plus the two fixtures).

timeout (scripts/test-timeout-path.sh): test_optin_hang.exe has exactly one
record, a FAIL with "timeout after <ms>ms"; its ring-3 marker was captured, so
it entered user mode; serial says it was reaped with the status named
TASK_EXIT_UTEST_TIMEOUT (not SIGKILL's -9 or any other); the counts reconcile.

Both modes require both artifacts to carry <run-id>. Exits 0 when everything
holds and prints each problem otherwise.
"""
import json
import re
import sys
import xml.etree.ElementTree as ET

SWAP = "test_optin_swap.exe"
VICTIM = "test_optin_victim.exe"
VICTIM_MARK = "[OPTIN-VICTIM]"
REPLACED = "planned binary replaced"
HANG = "test_optin_hang.exe"
HANG_MARK = "[OPTIN-HANG] in ring 3"
TIMEOUT_NAME = "TASK_EXIT_UTEST_TIMEOUT"


def load(json_path, xml_path, run_id, expected_total, problems):
    try:
        with open(json_path) as fh:
            doc = json.load(fh)
    except (OSError, ValueError) as e:
        problems.append(f"{json_path} unreadable: {e}")
        return None, None
    try:
        root = ET.parse(xml_path).getroot()
    except (OSError, ET.ParseError) as e:
        problems.append(f"{xml_path} unreadable: {e}")
        return doc, None
    if (doc.get("run_identity") or {}).get("run_id") != run_id:
        problems.append(f"JSON run_id is not {run_id}")
    if root.tag != "testsuite":
        problems.append(f"JUnit root is <{root.tag}>, not <testsuite>")
        return doc, None
    props = {p.get("name"): p.get("value") for p in root.iter("property")}
    if props.get("run_id") != run_id:
        problems.append(f"JUnit run_id is not {run_id}")
    s = doc.get("summary") or {}
    if s.get("aborted") is not False:
        problems.append(f"JSON summary.aborted={s.get('aborted')!r}, expected false")
    if s.get("not_run") != 0:
        problems.append(f"JSON summary.not_run={s.get('not_run')!r}, expected 0")
    cases = doc.get("testcases", [])
    if s.get("total") != expected_total or len(cases) != expected_total:
        problems.append(f"JSON total={s.get('total')!r} with {len(cases)} testcases, "
                        f"expected {expected_total}")
    return doc, root


def records(doc, root, name):
    js = [c for c in doc.get("testcases", []) if c.get("name") == name]
    xs = [c for c in root.findall("testcase") if c.get("name") == name]
    return js, xs


def check_identity(doc, root, slice_text, problems):
    js, xs = records(doc, root, SWAP)
    if [c.get("status") for c in js] != ["PASS"]:
        problems.append(f"JSON {SWAP} records {[c.get('status') for c in js]}, expected one PASS")
    js, xs = records(doc, root, VICTIM)
    if len(js) != 1 or js[0].get("status") != "ERROR" or js[0].get("reason") != REPLACED:
        problems.append(f"JSON {VICTIM} records "
                        f"{[(c.get('status'), c.get('reason')) for c in js]}, "
                        f"expected one ERROR {REPLACED!r}")
    errs = [x.find("error") for x in xs]
    if len(xs) != 1 or errs[0] is None or errs[0].get("message") != REPLACED:
        problems.append(f"JUnit {VICTIM} is not exactly one testcase with "
                        f"<error message={REPLACED!r}>")
    # Ring-3 stdout is captured into the record, not echoed to serial, so the
    # marker is looked for in both.
    if VICTIM_MARK in slice_text or any(VICTIM_MARK in (c.get("captured_output") or "")
                                        for c in js):
        problems.append("the replaced victim EXECUTED: its marker was published")


def check_timeout(doc, root, slice_text, timeout_ms, problems):
    reason = f"timeout after {timeout_ms}ms"
    js, xs = records(doc, root, HANG)
    if len(js) != 1 or js[0].get("status") != "FAIL" or js[0].get("reason") != reason:
        problems.append(f"JSON {HANG} records "
                        f"{[(c.get('status'), c.get('reason')) for c in js]}, "
                        f"expected one FAIL {reason!r}")
    elif HANG_MARK not in (js[0].get("captured_output") or ""):
        problems.append(f"{HANG} never reported reaching ring 3 (no {HANG_MARK!r} captured)")
    fails = [x.find("failure") for x in xs]
    if len(xs) != 1 or fails[0] is None or fails[0].get("message") != reason:
        problems.append(f"JUnit {HANG} is not exactly one testcase with "
                        f"<failure message={reason!r}>")
    reaped = re.findall(re.escape(HANG) + r": reaped with exit status (-?\d+) \((\w+)\)",
                        slice_text)
    if len(reaped) != 1:
        problems.append(f"serial has {len(reaped)} reap lines for {HANG}, expected 1")
    elif reaped[0][1] != TIMEOUT_NAME:
        problems.append(f"{HANG} was reaped with status {reaped[0][0]} ({reaped[0][1]}), "
                        f"not {TIMEOUT_NAME}")


def main(argv):
    mode = argv[1] if len(argv) > 1 else ""
    want = {"identity": 7, "timeout": 8}.get(mode)
    if want is None or len(argv) != want:
        print(__doc__.strip().splitlines()[2 if mode == "identity" else 3], file=sys.stderr)
        return 2
    json_path, xml_path, slice_path, run_id = argv[2:6]
    try:
        expected_total = int(argv[6])
        timeout_ms = int(argv[7]) if mode == "timeout" else 0
    except ValueError:
        print(f"[OPTIN-PATH] FAIL: a count is not a number: {argv[6:]}", file=sys.stderr)
        return 2
    problems = []
    try:
        with open(slice_path, encoding="utf-8", errors="replace") as fh:
            slice_text = fh.read()
    except OSError as e:
        problems.append(f"{slice_path} unreadable: {e}")
        slice_text = ""
    doc, root = load(json_path, xml_path, run_id, expected_total, problems)
    if doc is not None and root is not None:
        if mode == "identity":
            check_identity(doc, root, slice_text, problems)
        else:
            check_timeout(doc, root, slice_text, timeout_ms, problems)
    for p in problems:
        print(f"[OPTIN-PATH] FAIL ({mode}): {p}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
