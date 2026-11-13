#!/usr/bin/env python3
"""End-to-end fixtures for .claude/hooks/todo_graph_auto_rewrite.py.

Each class runs the REAL hook (stdin JSON, as the harness sends it) inside a
throwaway git repository whose scripts/todo-graph/validate.py is a fake that
prints a recorded output and exits with the class's code. The contract is the one
in validate.py's --fix-line-numbers header: rc 0 is silent; rc 1 (incomplete), rc 2 (refused) and
any other failure each produce a systemMessage that says which. The
unresolvable-only and missing-only rc 1 runs are the cases the hook used to
misreport: it keyed on the word "ambiguous", which the summary line always
contains (as "0 ambiguous"), so it called every incomplete run an ambiguous
rewrite and stayed silent on a refusal or a crash.

    python3 scripts/tests/test_todo_graph_auto_rewrite.py
"""
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HOOK = ROOT / ".claude" / "hooks" / "todo_graph_auto_rewrite.py"

SUMMARY = "[validate.py] fix-line-numbers: 0 update(s), {a} ambiguous, {u} unresolvable target(s), {m} missing item(s); mode=WRITE"
CLASSES = [
    # label, rc, output, expected substring (None = silent)
    ("every stamp correct is silent", 0, SUMMARY.format(a=0, u=0, m=0), None),
    ("an ambiguous stamp is incomplete", 1,
     "[validate.py] FAIL fix-line-numbers: ambiguous item 'x' in t.md\n" + SUMMARY.format(a=1, u=0, m=0),
     "left stamps it could not repair"),
    ("an unresolvable target alone is incomplete", 1,
     "[validate.py] FAIL fix-line-numbers: unresolvable XREF target 'nope.md'\n" + SUMMARY.format(a=0, u=1, m=0),
     "left stamps it could not repair"),
    ("a missing item alone is incomplete", 1,
     "[validate.py] FAIL fix-line-numbers: item 'gone' not found in t.md\n" + SUMMARY.format(a=0, u=0, m=1),
     "left stamps it could not repair"),
    ("a refusal says nothing changed", 2,
     "[validate.py] REFUSED: duplicate resolver key (00, 4)", "REFUSED before repairing any stamp"),
    ("a crash is a failure, not silence", 3, "Traceback: boom", "failed (exit 3)"),
]


def run_class(rc, output):
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["git", "init", "-q", tmp], check=True, timeout=30)
        fake = pathlib.Path(tmp, "scripts", "todo-graph", "validate.py")
        fake.parent.mkdir(parents=True)
        fake.write_text(f"import sys\nsys.stdout.write({output!r})\nsys.exit({rc})\n")
        event = {"tool_input": {"file_path": os.path.join(tmp, "todo", "x", "TODO-01.md")}}
        proc = subprocess.run([sys.executable, str(HOOK)], input=json.dumps(event),
                              capture_output=True, text=True, cwd=tmp, timeout=60)
    msg = json.loads(proc.stdout)["systemMessage"] if proc.stdout.strip() else None
    return proc.returncode, msg


def main():
    bad = 0
    for label, rc, output, want in CLASSES:
        code, msg = run_class(rc, output)
        ok = code == 0 and ((want is None and msg is None) or (want and msg and want in msg))
        if ok and rc == 1 and "ambiguous item" not in output:
            ok = "ambiguous item-name" not in msg  # not misreported as an ambiguity
        bad += not ok
        print(f"{'PASS' if ok else 'FAIL'} {label} (hook exit {code}, message {msg!r:.100})")
    # A timeout cannot be reached end-to-end inside the 15 s budget cheaply, so
    # the message it produces is checked on the function the hook calls.
    spec = importlib.util.spec_from_file_location("hook", HOOK)
    hook = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hook)
    msg = hook.message_for(None, "", "", timed_out=True)
    ok = bool(msg) and "did not finish" in msg
    bad += not ok
    print(f"{'PASS' if ok else 'FAIL'} a timeout is reported as unfinished")
    total = len(CLASSES) + 1
    print(f"{total - bad}/{total} passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
