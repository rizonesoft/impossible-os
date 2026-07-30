#!/usr/bin/env python3
# Protects the v04 runner item "the self-modify boundary is drawn by DIRECTORY,
# not by blast radius". The guard blocks an UNATTENDED edit to build/verification
# machinery that sits on the receipt surface, and must stay silent on everything
# else -- especially ordinary src/ and user/ work, which a wider directory
# denylist would have caught along with it.
import importlib.util
import json
import os
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
HOOK = HERE.parents[2] / ".claude/hooks/receipt_surface_guard.py"
ROOT = HERE.parents[2]


def _load():
    spec = importlib.util.spec_from_file_location("rsg", HOOK)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def _run(cmd_env, payload):
    env = dict(os.environ)
    env.update(cmd_env)
    env["CLAUDE_PROJECT_DIR"] = str(ROOT)
    r = subprocess.run([sys.executable, str(HOOK)], input=json.dumps(payload),
                       capture_output=True, text=True, env=env)
    return r.returncode, r.stderr


def test_embedded_selftest_passes():
    r = subprocess.run([sys.executable, str(HOOK), "--selftest"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


def test_the_five_files_from_the_live_incident():
    """2026-07-29: a legitimate refactor left these modified, all outside the
    old directory boundary. build.sh / Makefile / the generator are machinery on
    the receipt surface; the two GENERATED headers are outputs and stay
    ordinary, because gating them would block routine ABI work."""
    m = _load()
    bi = m._build_input_paths(ROOT)
    assert bi is not None, "BUILD_INPUT_PATHS must be importable from receipts.py"
    for rel in ("scripts/build.sh", "Makefile", "scripts/gen-user-abi.py"):
        assert m.is_receipt_machinery(rel, bi), rel
    for rel in ("include/kernel/abi_hash.h", "user/include/abi_numbers.h"):
        assert not m.is_receipt_machinery(rel, bi), rel


def test_ordinary_work_is_never_gated():
    """The reason this is a receipt-surface gate and not a bigger denylist."""
    m = _load()
    bi = m._build_input_paths(ROOT)
    for rel in ("src/kernel/sched/task.c", "src/kernel/mm/vmm.c",
                "include/kernel/types.h", "user/lib/crt_init.c",
                "resources/fonts/x.ttf", "tools/x/main.c",
                "scripts/test.sh", "scripts/lint.sh",
                "todo/00-infrastructure/TODO-04-usermode-test-framework.md",
                "docs/infrastructure/mcp-usage.md"):
        assert not m.is_receipt_machinery(rel, bi), rel


def test_gate_is_unattended_only_and_fails_open():
    build_sh = str(ROOT / "scripts/build.sh")
    payload = {"tool_name": "Edit", "tool_input": {"file_path": build_sh}}
    # Interactive: never gated -- an operator editing build.sh IS the repair path.
    rc, _ = _run({"OVERNIGHT_SEQUENCER_RUN": ""}, payload)
    assert rc == 0
    # Unattended: blocked, and the message must say FILE IT rather than read as
    # a refusal of the work.
    rc, err = _run({"OVERNIGHT_SEQUENCER_RUN": "1"}, payload)
    assert rc == 2, err
    assert "file it" in err.lower() and "overnight-runner-improvements-vNN" in err
    # Documented opt-out.
    rc, _ = _run({"OVERNIGHT_SEQUENCER_RUN": "1",
                  "RECEIPT_SURFACE_OVERRIDE": "1"}, payload)
    assert rc == 0
    # Ordinary source under the same unattended run stays free.
    rc, _ = _run({"OVERNIGHT_SEQUENCER_RUN": "1"},
                 {"tool_name": "Edit",
                  "tool_input": {"file_path": str(ROOT / "src/kernel/mm/vmm.c")}})
    assert rc == 0
    # Malformed payload must fail OPEN -- a guard that wedges the run it
    # protects is worse than the risk it covers.
    env = dict(os.environ, OVERNIGHT_SEQUENCER_RUN="1", CLAUDE_PROJECT_DIR=str(ROOT))
    r = subprocess.run([sys.executable, str(HOOK)], input="not json",
                       capture_output=True, text=True, env=env)
    assert r.returncode == 0


def test_list_is_imported_not_retyped():
    """The item's own instruction: reuse the list the repo already maintains,
    do not invent a second one that will drift from it."""
    src = HOOK.read_text(encoding="utf-8")
    assert "BUILD_INPUT_PATHS" in src
    assert "receipts.py" in src
    # The literal receipt-input list must NOT be duplicated in the hook.
    assert '"resources", "tools"' not in src, "BUILD_INPUT_PATHS re-typed"
    m = _load()
    real = m._build_input_paths(ROOT)
    assert "scripts/build.sh" in real and "Makefile" in real


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
    print("PASS: receipt-surface guard (v04 self-modify boundary)")
