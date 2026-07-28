#!/usr/bin/env python3
# The SKIP_* opt-out silently vanished when a command carried a heredoc whose
# body contained an ASCII apostrophe (live, 2026-07-28, twice in five minutes).
# `_scan_inline` ran shlex.split() over the WHOLE command and returned {} on
# ValueError; an apostrophe in a heredoc body is an unbalanced quote, so the
# gate BLOCKED while its own message told the caller to set the variable that
# was already set.
#
# Blast radius was every gate sharing this helper -- SKIP_REVIEW_HOOK,
# SKIP_SKILL_STEP_BLOCK, SKIP_PHASE1_BLOCK, SKIP_RUNNER_SUITE, SKIP_HOOK_AUDIT
# and the SKIP_REVIEW_PIPELINE alias -- and the triggering shape is how prose
# reaches a command at all: commit messages, TODO edits, doc writes.
#
# BOTH directions are load-bearing and the NEGATIVE half matters more: a
# scanner that over-detects would hand every gate a universal escape hatch. So
# the mention-only and no-opt-out cases are tested as hard as the recovery.
import importlib.util
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
HELPER = HERE.parents[2] / ".claude/hooks/_skip_env.py"
FAILS = []

K = ("SKIP_REVIEW_HOOK", "SKIP_RUNNER_SUITE")
APOS = "'"                      # ASCII apostrophe -- the exact trigger


def check(name, cond):
    if not cond:
        FAILS.append(name)


def _load():
    spec = importlib.util.spec_from_file_location("skip_env_t", HELPER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _heredoc(prefix, body):
    return f"{prefix}tee /tmp/f.md > /dev/null <<'EOF'\n{body}\nEOF\necho done"


def test_the_reported_shape_is_recovered():
    m = _load()
    got = m.read_skip_envs(
        _heredoc("SKIP_REVIEW_HOOK=1 ", f"v01{APOS}s lesson, and the runner{APOS}s prose"),
        keys=K, fallback_to_environ=False)
    check("heredoc + apostrophe recovered", got.get("SKIP_REVIEW_HOOK") == "1")


def test_shapes_that_already_worked_still_work():
    m = _load()

    def got(cmd):
        return m.read_skip_envs(cmd, keys=K, fallback_to_environ=False)

    check("plain command", got("SKIP_REVIEW_HOOK=1 git commit -m x").get("SKIP_REVIEW_HOOK") == "1")
    check("heredoc, plain body",
          got(_heredoc("SKIP_REVIEW_HOOK=1 ", "plain body")).get("SKIP_REVIEW_HOOK") == "1")
    check("apostrophe in dquotes, no heredoc",
          got(f'SKIP_REVIEW_HOOK=1 echo "it{APOS}s fine"').get("SKIP_REVIEW_HOOK") == "1")
    check("two vars + apostrophe heredoc",
          got(_heredoc("SKIP_RUNNER_SUITE=1 SKIP_REVIEW_HOOK=1 ",
                       f"it{APOS}s here")).get("SKIP_RUNNER_SUITE") == "1")
    check("behind a wrapper word",
          got(f"env SKIP_REVIEW_HOOK=1 tee f <<{APOS}E{APOS}\nit{APOS}s\nE").get(
              "SKIP_REVIEW_HOOK") == "1")


def test_it_must_not_over_detect():
    """The dangerous direction. An opt-out is a PREFIX construct; nothing past
    the first real token may set one, or every gate gains a free bypass."""
    m = _load()

    def got(cmd):
        return m.read_skip_envs(cmd, keys=K, fallback_to_environ=False)

    check("mention only is not an opt-out", got("echo SKIP_REVIEW_HOOK=1") == {})
    check("mention inside a heredoc body is not an opt-out",
          got(_heredoc("", f"SKIP_REVIEW_HOOK=1 is what it{APOS}s called")) == {})
    check("apostrophe but no opt-out",
          got(f"git commit -m \"it{APOS}s fine\"") == {})
    check("mid-command assignment is not a prefix",
          got("git commit && SKIP_REVIEW_HOOK=1") == {})
    check("a different key is not returned", got("SKIP_SOMETHING_ELSE=1 git x") == {})
    check("value 0 is reported as 0, not swallowed",
          got("SKIP_REVIEW_HOOK=0 git x").get("SKIP_REVIEW_HOOK") == "0")


def test_prefix_walker_directly():
    m = _load()
    ks = set(K)
    check("walker: recovers after wrappers",
          m._scan_prefix_no_shlex("sudo env SKIP_REVIEW_HOOK=1 cmd", ks).get(
              "SKIP_REVIEW_HOOK") == "1")
    check("walker: stops at first real token",
          m._scan_prefix_no_shlex("echo SKIP_REVIEW_HOOK=1", ks) == {})
    check("walker: empty command", m._scan_prefix_no_shlex("", ks) == {})
    check("walker: flag-like token ends the walk",
          m._scan_prefix_no_shlex("--flag=x SKIP_REVIEW_HOOK=1 cmd", ks) == {})


if __name__ == "__main__":
    test_the_reported_shape_is_recovered()
    test_shapes_that_already_worked_still_work()
    test_it_must_not_over_detect()
    test_prefix_walker_directly()
    if FAILS:
        for f in FAILS:
            print("FAIL:", f)
        raise SystemExit(1)
    print("test_skip_env_prefix OK (heredoc recovery + no over-detection)")
