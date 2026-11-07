#!/usr/bin/env python3
"""The pre-push host-gate step: every listed gate passes on this tree, and a broken
or missing gate refuses."""
import pathlib
import re
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPO / "scripts/host-gates.sh"
GATES = re.findall(r"tools/[\w-]+/\w+\.sh", SCRIPT.read_text())


def _fixture(d, failing=None, missing=None):
    root = pathlib.Path(d)
    for gate in GATES:
        if gate == missing:
            continue
        p = root / gate
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("exit 1\n" if gate == failing else "exit 0\n")
    return root


def _run(root):
    return subprocess.run(["bash", str(SCRIPT), str(root)], capture_output=True,
                          text=True)


def test_all_green_passes():
    with tempfile.TemporaryDirectory() as d:
        assert _run(_fixture(d)).returncode == 0


def test_refusal_a_failing_gate_is_named():
    with tempfile.TemporaryDirectory() as d:
        r = _run(_fixture(d, failing=GATES[1]))
        assert r.returncode == 1 and GATES[1] in r.stderr, r


def test_refusal_a_missing_gate_is_not_a_skip():
    with tempfile.TemporaryDirectory() as d:
        r = _run(_fixture(d, missing=GATES[0]))
        assert r.returncode == 1 and "missing" in r.stderr, r


def test_every_listed_gate_exists_and_prepush_fires_on_sources():
    assert len(GATES) == 6
    assert all((REPO / g).is_file() for g in GATES)
    hook = (REPO / ".githooks/pre-push").read_text()
    assert "scripts/host-gates.sh" in hook
    pattern = re.search(r"HOST_GATE_PATHS_RE='([^']+)'", hook).group(1)
    for path in ("src/boot/uefi/boot_entries_parser.c", "include/kernel/x.h",
                 "tools/boot-header-tests/run.sh"):
        assert re.search(pattern, path), path
    assert not re.search(pattern, "docs/x.md")
    assert not re.search(pattern, "todo/x.md")


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")
