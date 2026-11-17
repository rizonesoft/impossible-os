#!/usr/bin/env python3
"""The Bash safety guards see a guarded command behind shell grammar.

WHY (2026-11-17): attended_repair_guard and runner_bash_guard took a segment's
first word as its command, so `if true; then git reset --hard; fi` was read as
the command `then` and allowed. The same held for `do`, `!`, `{`, `time`, a
subshell paren and a `case` arm, all of which bash runs. Found by the plan pass
for the shell-walk deletion (automation hardening, grammar-accurate shell segmentation).
"""
from __future__ import annotations

import importlib.util
import pathlib

REPO = pathlib.Path(__file__).resolve().parents[3]


def _load(name):
    spec = importlib.util.spec_from_file_location(name, str(REPO / f".claude/hooks/{name}.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


ATTENDED = _load("attended_repair_guard")
RUNNER = _load("runner_bash_guard")

# Each wraps a command both guards refuse on its own.
SHAPES = [
    "if true; then {c}; fi",
    "if {c}; then :; fi",
    "if false; then :; elif true; then {c}; fi",
    "if false; then :; else {c}; fi",
    "while :; do {c}; done",
    "until false; do {c}; done",
    "for f in a; do {c}; done",
    "! {c}",
    "{{ {c}; }}",
    "({c})",
    "(true; {c})",
    "time {c}",
    "time -p {c}",
    "case x in x) {c};; esac",
    "case x in (x) {c};; esac",
    "case x in y) :;; x) {c};; esac",
    "case x in y|x) {c};; esac",
    "if true; then FOO=1 {c}; fi",
    "if true; then timeout 5 {c}; fi",
    "case x in x){c};; esac",
    "case x in y) :;; x){c};; esac",
    "case x in (x){c};; esac",
    "({c}) 2>/dev/null",
    "({c}) >/dev/null 2>&1",
    "(env {c})",
    "(FOO=1 {c})",
    "(command {c})",
    "case x in x)env {c};; esac",
    "case x in (x)FOO=1 {c};; esac",
    "if true; then (sudo {c}); fi",
    "if true; then >/dev/null {c}; fi",
    "2>/dev/null {c}",
    "> /dev/null {c}",
    "git 2>/dev/null reset --hard",
    "git > /dev/null reset --hard",
    ">& 2 {c}",
    "git >& 2 reset --hard",
    "<& 0 {c}",
    "git>/dev/null reset --hard",
    "git reset>/dev/null --hard",
    "git reset --hard>/dev/null",
    "git reset&>/dev/null --hard",
    "git reset --hard&>/dev/null",
    "git&>>log reset --hard",
]
REFUSED = "git reset --hard"


def test_attended_guard_sees_through_shell_grammar():
    assert ATTENDED._scan(REFUSED), "control: the bare command must be refused"
    missed = [s for s in SHAPES if not ATTENDED._scan(s.format(c=REFUSED))]
    assert not missed, f"attended_repair_guard allowed: {missed}"


def test_runner_guard_sees_through_shell_grammar():
    assert RUNNER._scan_command(REFUSED), "control: the bare command must be refused"
    missed = [s for s in SHAPES if not RUNNER._scan_command(s.format(c=REFUSED))]
    assert not missed, f"runner_bash_guard allowed: {missed}"


def test_harmless_compounds_stay_allowed():
    for cmd in ("if true; then git status; fi", "case x in x) echo git reset --hard;; esac",
                "while :; do git log -1; done", "(git diff --stat)", "time git status",
                "if git diff --quiet; then echo clean; fi", "git status 2>/dev/null",
                ">/dev/null git log -1"):
        assert not ATTENDED._scan(cmd), f"attended_repair_guard refused {cmd!r}"
        assert not RUNNER._scan_command(cmd), f"runner_bash_guard refused {cmd!r}"


def test_a_gh_mutation_behind_grammar_is_refused():
    for shape in ("(command gh pr merge 123)", "case x in x)env gh pr merge 1;; esac", "if :; then gh pr merge 1; fi"):
        assert RUNNER._scan_command(shape), f"runner_bash_guard allowed {shape!r}"


def test_a_long_quoted_pattern_scales_linearly():
    """One word holding 220,000 `)` took 6.5 s when each `)` re-sliced the word."""
    import time
    cmd = "case x in '" + ")" * 220000 + "') echo ok;; esac"
    t0 = time.monotonic()
    ATTENDED._scan(cmd)
    RUNNER._scan_command(cmd)
    took = time.monotonic() - t0
    assert took < 2.5, f"two guard scans of a 220 KB pattern took {took:.2f} s"


def test_nested_cases_scale_linearly():
    """A 420 KB nested-case command took 6.4 s (the hook timeout is 5 s) when each
    case rescanned and copied the rest of the tokens."""
    import time
    cmd = "case x in x) " * 1000 + "echo " + "x " * 200000 + ";; esac" * 1000
    t0 = time.monotonic()
    ATTENDED._scan(cmd)
    RUNNER._scan_command(cmd)
    took = time.monotonic() - t0
    assert took < 2.5, f"two guard scans of a 420 KB nested case took {took:.2f} s"


if __name__ == "__main__":
    test_attended_guard_sees_through_shell_grammar()
    test_runner_guard_sees_through_shell_grammar()
    test_harmless_compounds_stay_allowed()
    test_nested_cases_scale_linearly()
    test_a_gh_mutation_behind_grammar_is_refused()
    test_a_long_quoted_pattern_scales_linearly()
    print("PASS: guards see through shell grammar")
