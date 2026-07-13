#!/usr/bin/env python3
# Protects I4 (2026-07-13 findings): finding/decision reuse was structurally dead.
# Of 56 finding-triage records, 43 had EMPTY evidence and NONE stored a todo, so
# `finding-ledger.py list --todo` could never match and the decision registry
# drifted staler than the TODO cache -- the review re-triaged settled findings for
# zero source changes. Fix: require --todo + --evidence on record (so list --todo
# works), and auto-rebuild the decision registry when build/todo-cache.json is
# newer than the index.
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import time
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
LEDGER = HERE.parent / "finding-ledger.py"
REG = HERE.parent / "decision-registry.py"


def _repo(d):
    root = pathlib.Path(d)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / "src").mkdir()
    (root / "src/x.c").write_text("int x(void){return 1;}\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    return root


def _ledger(root, *args):
    return subprocess.run([sys.executable, str(LEDGER), *args],
                          cwd=str(root), capture_output=True, text=True)


def test_record_requires_todo():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        r = _ledger(root, "record", "adversarial", "HIGH", "src/x.c:1",
                    "--decision", "fix", "--evidence", "real bug", "a title")
        assert r.returncode == 2 and "--todo" in r.stderr, (r.returncode, r.stderr)


def test_record_requires_evidence():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        r = _ledger(root, "record", "adversarial", "HIGH", "src/x.c:1",
                    "--decision", "fix", "--todo", "todo/TODO-1.md", "a title")
        assert r.returncode == 2 and "--evidence" in r.stderr, (r.returncode, r.stderr)


def test_record_stores_todo_and_list_filters():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        r = _ledger(root, "record", "adversarial", "HIGH", "src/x.c:1",
                    "--decision", "reject", "--todo", "todo/TODO-7.md",
                    "--evidence", "guarded at src/x.c:1", "the title")
        assert r.returncode == 0, r.stderr
        assert json.loads(r.stdout)["todo"] == "todo/TODO-7.md"
        # list --todo matches the stored section...
        r = _ledger(root, "list", "--todo", "todo/TODO-7.md")
        assert r.returncode == 0 and "todo/TODO-7.md" in r.stdout, r.stdout
        # ...and excludes a different one.
        r = _ledger(root, "list", "--todo", "todo/OTHER.md")
        assert r.stdout.strip() == "", r.stdout


def test_registry_auto_refresh_logic():
    spec = importlib.util.spec_from_file_location("dreg", REG)
    dreg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(dreg)
    calls = []
    dreg.build = lambda root: (calls.append(root), 0)[1]
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)
        (root / "build").mkdir()
        idx = root / dreg.INDEX_REL
        cache = root / "build" / "todo-cache.json"
        # No cache -> nothing to compare against -> no rebuild.
        dreg._auto_refresh_if_stale(root)
        assert calls == []
        # Cache present, index absent -> rebuild.
        cache.write_text("{}")
        dreg._auto_refresh_if_stale(root)
        assert len(calls) == 1
        # Index newer than cache -> no rebuild.
        idx.write_text("x")
        now = time.time()
        os.utime(idx, (now, now))
        os.utime(cache, (now - 100, now - 100))
        dreg._auto_refresh_if_stale(root)
        assert len(calls) == 1
        # Cache advanced past the index -> rebuild.
        os.utime(cache, (now + 100, now + 100))
        dreg._auto_refresh_if_stale(root)
        assert len(calls) == 2


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
    print("PASS: finding-decision-reuse (I4)")
