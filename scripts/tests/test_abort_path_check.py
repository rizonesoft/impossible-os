#!/usr/bin/env python3
"""Fixtures for scripts/lib/abort_path_check.py (the abort-path proof).

The checker must accept only the artifacts of the run it was given that
describe a smoke abort, and must refuse the near misses the proof could be
fooled by: an artifact-pipeline refusal document (an <error> with no
suite-abort record, the shape scripts/test.sh utest_xml_refusal_doc writes),
another run's artifacts, a not_run that does not match, and a fixture that
did not fail.

    python3 scripts/tests/test_abort_path_check.py
"""
import importlib.util
import json
import os
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "abort_path_check", HERE.parent / "lib" / "abort_path_check.py")
apc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(apc)

FX = "test_optin_smoke_abort.exe"
RUN = "20261113T000000Z-1-abcdef01"


def run_json(run_id=RUN, aborted=True, not_run=17, status="FAIL"):
    return {"run_identity": {"run_id": run_id},
            "summary": {"aborted": aborted, "not_run": not_run},
            "testcases": [{"name": FX, "status": status}]}


def abort_xml(run_id=RUN, not_run=17, fixture_failure=True):
    fx = '<failure message="exit=1"/>' if fixture_failure else ""
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="impossible-os-usermode" tests="2" failures="1" errors="1">
  <properties>
    <property name="aborted" value="true"/>
    <property name="not_run" value="{not_run}"/>
    <property name="run_id" value="{run_id}"/>
  </properties>
  <testcase name="suite-abort" classname="infrastructure">
    <error message="suite aborted (smoke failed); {not_run} binaries never ran"/>
  </testcase>
  <testcase name="{FX}" classname="smoke">{fx}</testcase>
</testsuite>
"""


# The shape of utest_xml_refusal_doc in scripts/test.sh, under this run's id.
REFUSAL_XML = f"""<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="impossible-os-usermode" tests="1" failures="0" skipped="0" errors="1" time="0">
  <properties>
    <property name="aborted" value="true"/>
    <property name="not_run" value="0"/>
    <property name="run_id" value="{RUN}"/>
  </properties>
  <testcase name="artifact-pipeline" classname="infrastructure">
    <error message="JUnit assembly failed"/>
  </testcase>
</testsuite>
"""


def problems(json_doc, xml_text):
    with tempfile.TemporaryDirectory() as d:
        jp, xp = os.path.join(d, "r.json"), os.path.join(d, "r.xml")
        with open(jp, "w") as fh:
            json.dump(json_doc, fh)
        with open(xp, "w") as fh:
            fh.write(xml_text)
        return apc.check(jp, xp, FX, 17, RUN)


CASES = [
    ("the real abort artifacts pass", run_json(), abort_xml(), False),
    ("a pipeline-refusal XML with valid abort JSON fails", run_json(), REFUSAL_XML, True),
    ("JSON from another run fails", run_json(run_id="other"), abort_xml(), True),
    ("XML from another run fails", run_json(), abort_xml(run_id="other"), True),
    ("a JSON not_run that does not match fails", run_json(not_run=16), abort_xml(), True),
    ("an XML not_run that does not match fails", run_json(), abort_xml(not_run=16), True),
    ("a run that did not abort fails", run_json(aborted=False), abort_xml(), True),
    ("a fixture that passed fails (JSON)", run_json(status="PASS"), abort_xml(), True),
    ("a fixture with no <failure> fails (XML)", run_json(), abort_xml(fixture_failure=False), True),
    ("an unparseable XML fails", run_json(), "<testsuite", True),
]


def main():
    bad = 0
    for label, j, x, want_fail in CASES:
        got = problems(j, x)
        ok = bool(got) == want_fail
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}" + ("" if ok else f" (problems={got})"))
        bad += not ok
    print(f"abort-path check fixtures: {len(CASES) - bad} passed, {bad} failed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
