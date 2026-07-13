#!/usr/bin/env python3
# Protects P3.1 (2026-07-12 plan): the split predictor missed exactly-8-item
# ABI/SSDT sections (`abi_impact and open_items > 8`), and a SPLIT-RECOMMENDED
# verdict could be overridden by a free-form "cohesive". Fix: ABI threshold
# `>= 6`, and a STRUCTURED waiver (est_files/subsystems/est_tests/context_budget/
# rationale) is required to override.
import importlib.util
import json
import subprocess
import sys
import tempfile
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
MANIFEST = HERE.parent / "section-manifest.py"


def _load():
    sys.path.insert(0, str(MANIFEST.parent))
    spec = importlib.util.spec_from_file_location("section_manifest", MANIFEST)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _repo_with_todo(d, items):
    root = pathlib.Path(d)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / "src/kernel/nt").mkdir(parents=True)
    (root / "src/kernel/nt/env.c").write_text("int e(void){return 0;}\n")
    (root / "todo").mkdir()
    body = ["# Test TODO", "", "## 1. SSDT env syscalls (ABI impact)",
            "Touches `src/kernel/nt/env.c` and the SSDT shadow table.", ""]
    body += [f"- [ ] item {i}" for i in range(items)]
    (root / "todo/TODO-99.md").write_text("\n".join(body) + "\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    return root


def _manifest(root):
    r = subprocess.run(
        [sys.executable, str(MANIFEST), "todo/TODO-99.md", "1",
         "--project", str(root)], cwd=str(root), capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


def test_abi_8_item_section_now_splits():
    with tempfile.TemporaryDirectory() as d:
        root = _repo_with_todo(d, items=8)      # exactly 8 -> old `> 8` missed it
        cx = _manifest(root)["complexity"]
        assert "SPLIT-RECOMMENDED" in cx["verdict"], cx
        assert ">=6 ABI gate" in cx["verdict"], cx
        assert cx["waiver_required"] is True


def test_small_abi_section_fits():
    with tempfile.TemporaryDirectory() as d:
        root = _repo_with_todo(d, items=3)      # below the >=6 ABI gate
        cx = _manifest(root)["complexity"]
        assert cx["verdict"] == "fits-one-context", cx
        assert cx["waiver_required"] is False


def test_validate_split_waiver_structured_only():
    mod = _load()
    good = {"est_files": 3, "subsystems": ["src/kernel"], "est_tests": 4,
            "context_budget": "120K", "rationale": "one cohesive syscall family"}
    ok, missing = mod.validate_split_waiver(good)
    assert ok, missing
    # a free-form string is NOT a structured waiver
    ok, _ = mod.validate_split_waiver("cohesive")
    assert not ok
    # missing a field
    bad = dict(good); del bad["est_tests"]
    ok, missing = mod.validate_split_waiver(bad)
    assert not ok and "est_tests" in missing, missing
    # a bool is not an int estimate
    b2 = dict(good); b2["est_files"] = True
    ok, missing = mod.validate_split_waiver(b2)
    assert not ok and "est_files" in missing, missing
    # empty list / blank string rejected
    b3 = dict(good); b3["subsystems"] = []
    assert not mod.validate_split_waiver(b3)[0]


def test_waiver_check_cli():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        good = root / "w.json"
        good.write_text(json.dumps(
            {"est_files": 2, "subsystems": ["src/kernel"], "est_tests": 3,
             "context_budget": "100K", "rationale": "cohesive"}))
        r = subprocess.run([sys.executable, str(MANIFEST), "waiver-check", str(good)],
                           capture_output=True, text=True)
        assert r.returncode == 0 and json.loads(r.stdout)["ok"] is True, r.stdout
        bad = root / "b.json"
        bad.write_text(json.dumps({"rationale": "cohesive"}))
        r = subprocess.run([sys.executable, str(MANIFEST), "waiver-check", str(bad)],
                           capture_output=True, text=True)
        assert r.returncode == 1 and json.loads(r.stdout)["ok"] is False, r.stdout


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
    print("PASS: split-predictor (P3.1)")
