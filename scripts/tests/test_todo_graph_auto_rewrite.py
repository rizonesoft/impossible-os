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

RAISE = object()
LONG_LIST = ", ".join(f"todo/02-kernel-core/TODO-{n:02d}-a-long-subject-name.md" for n in range(40))
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
    ("an early refusal is a refusal", 2,
     "[validate.py] REFUSED: duplicate resolver key (00, 4)", "REFUSED: "),
    ("a late refusal names the files already rewritten", 2,
     "[validate.py] REFUSED: todo/b.md changed during repair. ALREADY REWRITTEN: todo/a.md",
     "already be rewritten: ALREADY REWRITTEN: todo/a.md"),
    ("a late refusal keeps every rewritten path", 2,
     "[validate.py] REFUSED: todo/z.md changed during repair. ALREADY REWRITTEN: " + LONG_LIST,
     LONG_LIST),
    # RAISE makes the fake die on a real uncaught ImportError (exit 1).
    ("a Python crash is not an incomplete repair", 1, RAISE, "did not finish"),
    ("another exit is a failure, not silence", 3, "Traceback: boom", "failed (exit 3)"),
]


def run_class(rc, output):
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["git", "init", "-q", tmp], check=True, timeout=30)
        fake = pathlib.Path(tmp, "scripts", "todo-graph", "validate.py")
        fake.parent.mkdir(parents=True)
        if output is RAISE:
            fake.write_text("import no_such_module_for_this_fixture\n")
        else:
            fake.write_text(f"import sys\nsys.stdout.write({output!r})\nsys.exit({rc})\n")
        event = {"tool_input": {"file_path": os.path.join(tmp, "todo", "x", "TODO-01.md")}}
        proc = subprocess.run([sys.executable, install_hook(tmp)], input=json.dumps(event),
                              capture_output=True, text=True, cwd=tmp, timeout=60)
    msg = json.loads(proc.stdout)["systemMessage"] if proc.stdout.strip() else None
    return proc.returncode, msg


def install_hook(tree):
    """A copy of the hook inside `tree`: the hook acts only on worktrees of the repository it is installed in."""
    dest = pathlib.Path(tree, ".claude", "hooks", HOOK.name)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(HOOK.read_text())
    return str(dest)


def fake_validator(tree):
    fake = pathlib.Path(tree, "scripts", "todo-graph", "validate.py")
    fake.parent.mkdir(parents=True, exist_ok=True)
    fake.write_text("import pathlib, os\npathlib.Path(os.getcwd(), 'ran-here').write_text('x')\n")
    pathlib.Path(tree, "todo").mkdir(exist_ok=True)


def git(*args):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *args], check=True, timeout=30,
                   capture_output=True)


def repair_tree_choice():
    """(linked worktree repaired there, unrelated or unregistered tree left alone). The hook runs with the session's
    primary tree as its cwd. An edit in a linked worktree must be repaired in THAT worktree (on 2026-11-23 an edit
    in ../impossible-os-gh rewrote 22 primary-tree TODO files under a live run), and an edit in an unrelated
    checkout must run no code from it, and neither must a tree that points at this repository's git metadata
    without being one of its registered worktrees (plan review, 2026-11-23)."""
    with tempfile.TemporaryDirectory() as tmp:
        a, b, c = (os.path.join(tmp, n) for n in "abc")
        git("init", "-q", a)
        hook = install_hook(a)
        fake_validator(a)
        git("-C", a, "add", "-A")
        git("-C", a, "commit", "-qm", "base")
        git("-C", a, "worktree", "add", "-q", "--detach", b)
        git("init", "-q", c)
        fake_validator(c)
        d = os.path.join(tmp, "d")                     # an unregistered tree sharing a's metadata
        os.makedirs(d)
        pathlib.Path(d, ".git").write_text(f"gitdir: {os.path.join(a, '.git')}\n")
        fake_validator(d)
        results = []
        for tree in (b, c, d):
            event = {"tool_input": {"file_path": os.path.join(tree, "todo", "TODO-01.md")}}
            subprocess.run([sys.executable, hook], input=json.dumps(event),
                           capture_output=True, text=True, cwd=a, timeout=60)
            results.append(pathlib.Path(tree, "ran-here").exists() and not pathlib.Path(a, "ran-here").exists())
        return results[0], not any(pathlib.Path(t, "ran-here").exists() for t in (a, c, d))


def validator_gets_only_the_remaining_budget():
    """The harness gives this hook 20 s (.claude/settings.json). The git lookups and the validator share ONE
    deadline: when the lookups take most of it, the validator gets only what is left (performance review,
    2026-11-23: an 8 s lookup and a 13 s repair each fit their own limit and still overran the hook's)."""
    import io
    spec = importlib.util.spec_from_file_location("hook_budget", HOOK)
    hook = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hook)
    clock = [1000.0]
    seen = {}
    import types
    hook.time = types.SimpleNamespace(monotonic=lambda: clock[0])   # this module's clock only

    def check_output(cmd, **kw):
        clock[0] += hook.BUDGET_S - 1.0              # the lookups use all but one second of the budget
        seen.setdefault("lookup", kw.get("timeout"))
        if "worktree" in cmd:
            return f"worktree {os.path.realpath(tmp)}\n".encode()
        return f"{tmp}\n".encode()

    def run(cmd, **kw):
        seen["validator"] = kw.get("timeout")
        return subprocess.CompletedProcess(cmd, 0, "", "")

    with tempfile.TemporaryDirectory() as tmp:
        fake_validator(tmp)
        hook.subprocess = types.SimpleNamespace(check_output=check_output, run=run, DEVNULL=subprocess.DEVNULL,
                                                TimeoutExpired=subprocess.TimeoutExpired,
                                                CompletedProcess=subprocess.CompletedProcess)
        sys_stdin = sys.stdin
        try:
            sys.stdin = io.StringIO(json.dumps({"tool_input": {"file_path": os.path.join(tmp, "todo", "a.md")}}))
            out = io.StringIO()
            sys_stdout, sys.stdout = sys.stdout, out
            try:
                hook.main()
            finally:
                sys.stdout = sys_stdout
        finally:
            sys.stdin = sys_stdin
    lookup_ok = seen.get("lookup") is not None and seen["lookup"] <= hook.BUDGET_S
    validator = seen.get("validator")
    # Two lookups each used BUDGET_S - 1: the budget is spent before the validator, which must not run at all,
    # and the hook must say so rather than stay silent.
    return lookup_ok and validator is None and "budget" in out.getvalue()


def main():
    bad = 0
    budget = validator_gets_only_the_remaining_budget()
    bad += not budget
    print(f"{'PASS' if budget else 'FAIL'} the lookups and the validator share one deadline")
    linked, unrelated = repair_tree_choice()
    bad += (not linked) + (not unrelated)
    print(f"{'PASS' if linked else 'FAIL'} the repair runs in the linked worktree that holds the edited file")
    print(f"{'PASS' if unrelated else 'FAIL'} an edit in an unrelated repository runs none of its code")
    for label, rc, output, want in CLASSES:
        code, msg = run_class(rc, output)
        ok = code == 0 and ((want is None and msg is None) or (want and msg and want in msg))
        if ok and rc == 1 and (output is RAISE or "ambiguous item" not in output):
            ok = "ambiguous item-name" not in msg  # not misreported as an ambiguity
        if ok and (output is RAISE or "fix-line-numbers: " not in output):
            ok = "safe rewrites were applied" not in msg  # never claims an unfinished run repaired anything
        if ok and rc == 2:
            ok = "changed nothing" not in msg and "before repairing" not in msg
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
    total = len(CLASSES) + 4
    print(f"{total - bad}/{total} passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
