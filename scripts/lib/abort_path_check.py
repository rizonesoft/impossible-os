"""Check the artifacts of one abort-path run (scripts/test-abort-path.sh).

Usage: abort_path_check.py <test-results.json> <test-results.xml> <fixture>
       <expected-not-run> <run-id>

Exits 0 when both artifacts belong to run <run-id> and describe the abort:
the JSON summary has aborted true and not_run equal to <expected-not-run>,
and the fixture's testcase is FAIL; the JUnit file has aborted/not_run
properties to match, a `suite-abort` testcase carrying <error>, and the
fixture's testcase carrying <failure>. A pipeline-refusal document (an
<error> with none of that) does not pass. Prints each problem otherwise.
"""
import json
import sys
import xml.etree.ElementTree as ET


def check(json_path, xml_path, fixture, expected, run_id):
    problems = []
    try:
        with open(json_path) as fh:
            doc = json.load(fh)
    except (OSError, ValueError) as e:
        return [f"{json_path} unreadable: {e}"]
    if (doc.get("run_identity") or {}).get("run_id") != run_id:
        problems.append(f"JSON run_id is not {run_id}")
    s = doc.get("summary") or {}
    if s.get("aborted") is not True:
        problems.append(f"JSON summary.aborted={s.get('aborted')!r}")
    if s.get("not_run") != expected:
        problems.append(f"JSON summary.not_run={s.get('not_run')!r}, expected {expected}")
    status = {c.get("name"): c.get("status") for c in doc.get("testcases", [])}
    if status.get(fixture) != "FAIL":
        problems.append(f"JSON {fixture} status={status.get(fixture)!r}")

    try:
        root = ET.parse(xml_path).getroot()
    except (OSError, ET.ParseError) as e:
        return problems + [f"{xml_path} unreadable: {e}"]
    if root.tag != "testsuite":
        return problems + [f"JUnit root is <{root.tag}>, not <testsuite>"]
    props = {p.get("name"): p.get("value") for p in root.iter("property")}
    if props.get("run_id") != run_id:
        problems.append(f"JUnit run_id is not {run_id}")
    if props.get("aborted") != "true":
        problems.append(f"JUnit aborted={props.get('aborted')!r}")
    if props.get("not_run") != str(expected):
        problems.append(f"JUnit not_run={props.get('not_run')!r}, expected {expected}")
    cases = {c.get("name"): c for c in root.findall("testcase")}
    abort = cases.get("suite-abort")
    if abort is None or abort.find("error") is None:
        problems.append("JUnit has no suite-abort testcase with <error>")
    fx = cases.get(fixture)
    if fx is None or fx.find("failure") is None:
        problems.append(f"JUnit {fixture} testcase has no <failure>")
    return problems


def main(argv):
    if len(argv) != 6:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    try:
        expected = int(argv[4])
    except ValueError:
        print(f"[ABORT-PATH] FAIL: expected-not-run {argv[4]!r} is not a number", file=sys.stderr)
        return 2
    problems = check(argv[1], argv[2], argv[3], expected, argv[5])
    for p in problems:
        print(f"[ABORT-PATH] FAIL: {p}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
