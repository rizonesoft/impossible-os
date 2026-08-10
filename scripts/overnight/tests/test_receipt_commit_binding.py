#!/usr/bin/env python3
"""v13 carry, resolved 2026-08-10: the tooling receipt can attest the bytes
being PUSHED, not only the bytes currently on disk.

THE FILED PREMISE WAS WRONG AND THE SYMPTOM WAS REAL. The item read "the
pre-push receipt is not content-bound; bind it the way tooling-receipt.py
does". But `tooling-receipt.py:surface_key` already hashes every tracked and
untracked file on the tooling surface -- it was content-bound from the start,
and binding it TO THE WORKING TREE is exactly what produces the reported
symptom: the run backgrounds its ship push, keeps editing, and its own later
edits move the worktree key, so a receipt that validly attests the pushed
commit is refused and the ~6-minute suite runs inside the push.

So the fix is a SECOND QUESTION, not a second mechanism: `check --commit <sha>`.

THE REFUSAL DIRECTION IS THE POINT. A receipt that can be satisfied too easily
is a gate that is not running, so most of what follows asserts that things
still FAIL: a different commit, an unreadable commit, and -- the one that
matters -- a real change to the committed tooling surface.

GUARD vs PIN, measured rather than asserted (mutation check 2026-08-10:
`if commit:` neutered to `if False:` on a copy, this file run against it):

  * GUARD -- fails when the fix is reverted, so it is what proves the fix works:
      test_unrelated_later_edit_no_longer_voids_the_pushed_commit
  * PINS -- pass in BOTH directions, so they prove nothing about the fix and
    everything about what must not change around it. That is their job and they
    are labelled rather than mistaken for guards:
      test_a_real_committed_tooling_change_still_fails
      test_wrong_or_unreadable_commit_fails_closed
      test_no_commit_arg_preserves_original_semantics
      test_worktree_and_commit_keys_agree_on_a_clean_tree

A file of pins alone would go green over a fix that never shipped. A file of
guards alone would go green over a fix that shipped and broke the gate. Both
kinds are needed and they are not interchangeable.

THIS FILE HAS ALREADY GONE INERT ONCE, which is why `_clone` asserts its own
premise: the moment the subject under test was COMMITTED, the fixture's
copy-then-commit became a no-op and `git commit` failed outright. It failed
loudly that time. The dangerous version of the same drift is the one that
fails quietly.
"""
import importlib.util
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
RECEIPT = REPO / "scripts/tooling-receipt.py"


def _tr():
    spec = importlib.util.spec_from_file_location("tr_mod", RECEIPT)
    m = importlib.util.module_from_spec(spec)
    sys.modules["tr_mod"] = m
    spec.loader.exec_module(m)
    return m


def _git(root, *args, **kw):
    return subprocess.run(["git", "-C", str(root), *args],
                          capture_output=True, text=True, **kw)


def _check(root, *extra):
    r = subprocess.run(
        [sys.executable, str(RECEIPT), "check", "--project", str(root), *extra],
        capture_output=True, text=True)
    return r.returncode


def _clone():
    """A clone whose clean worktree genuinely equals its HEAD, carrying the
    receipt script UNDER TEST.

    The copy-then-commit is conditional, and that is not defensive coding -- it
    is the difference between a live fixture and an inert one. While the fix is
    uncommitted, the working copy differs from the clone's HEAD and must be
    committed or every key comparison below measures a tree that does not match
    its own HEAD. Once the fix IS committed, the copy is byte-identical and
    `git commit` fails with "nothing to commit" -- which is exactly how this
    test failed the moment its own subject landed. Commit only when something
    is actually staged; the invariant this guarantees (worktree == HEAD) holds
    either way, which is the only thing the fixture needs.
    """
    d = pathlib.Path(tempfile.mkdtemp(prefix="receipt-bind."))
    root = d / "c"
    subprocess.run(["git", "clone", "-q", "--no-hardlinks", str(REPO), str(root)],
                   check=True, capture_output=True)
    (root / "scripts/tooling-receipt.py").write_bytes(RECEIPT.read_bytes())
    (root / ".claude/state").mkdir(parents=True, exist_ok=True)
    _git(root, "add", "scripts/tooling-receipt.py", check=True)
    if _git(root, "diff", "--cached", "--quiet").returncode != 0:
        _git(root, "-c", "user.email=t@t", "-c", "user.name=t",
             "commit", "-q", "-m", "receipt under test", check=True)
    # The fixture's whole premise, asserted rather than assumed: a clean clone
    # must have nothing outstanding, or the key comparisons measure noise.
    assert not _git(root, "status", "--porcelain").stdout.strip(), \
        "fixture clone is not clean; every key comparison below would be noise"
    return root


def test_worktree_and_commit_keys_agree_on_a_clean_tree():
    """The two computations must produce the SAME key for the same bytes --
    otherwise --commit could never match and the feature is decorative."""
    tr, root = _tr(), _clone()
    head = _git(root, "rev-parse", "HEAD").stdout.strip()
    assert tr.surface_key(root) == tr.surface_key_at_commit(root, head)


def test_unrelated_later_edit_no_longer_voids_the_pushed_commit():
    """The reported symptom, fixed."""
    root = _clone()
    head = _git(root, "rev-parse", "HEAD").stdout.strip()
    subprocess.run([sys.executable, str(RECEIPT), "write", "--project", str(root)],
                   check=True, capture_output=True)
    assert _check(root) == 0, "clean tree should pass with no --commit"
    # The run's own later work, while the ship push is in flight.
    with open(root / "scripts/lint.sh", "a") as fh:
        fh.write("\n# later, unrelated edit\n")
    assert _check(root) == 1, "REFUSAL CONTROL: worktree moved, plain check must fail"
    assert _check(root, "--commit", head) == 0, \
        "the receipt still describes the pushed commit exactly; it must pass"


def test_a_real_committed_tooling_change_still_fails():
    """REFUSAL CONTROL, and the load-bearing one. If this ever passes, the gate
    has stopped gating: a tooling change would reach origin with no suite run."""
    root = _clone()
    subprocess.run([sys.executable, str(RECEIPT), "write", "--project", str(root)],
                   check=True, capture_output=True)
    with open(root / "scripts/lint.sh", "a") as fh:
        fh.write("\n# a REAL tooling change\n")
    _git(root, "add", "scripts/lint.sh", check=True)
    _git(root, "-c", "user.email=t@t", "-c", "user.name=t",
         "commit", "-q", "-m", "real tooling change", check=True)
    new_head = _git(root, "rev-parse", "HEAD").stdout.strip()
    assert _check(root, "--commit", new_head) == 1, \
        "a changed committed surface must NOT be attested by an older receipt"


def test_wrong_or_unreadable_commit_fails_closed():
    """REFUSAL CONTROL: every uncertain state means 'run the suite'."""
    tr, root = _tr(), _clone()
    subprocess.run([sys.executable, str(RECEIPT), "write", "--project", str(root)],
                   check=True, capture_output=True)
    with open(root / "scripts/lint.sh", "a") as fh:
        fh.write("\n# later edit\n")
    assert _check(root, "--commit", _git(root, "rev-parse", "HEAD~1").stdout.strip()) == 1
    assert _check(root, "--commit", "0" * 40) == 1
    assert _check(root, "--commit", "not-a-ref") == 1
    # And the underlying probe returns None rather than a hashable empty set --
    # an "empty surface" key would be stable and could MATCH another empty one.
    assert tr.surface_key_at_commit(root, "0" * 40) is None
    assert tr.surface_key_at_commit(root, "not-a-ref") is None


def test_no_commit_arg_preserves_original_semantics():
    """The flag is additive. Without it, `check` must behave exactly as before,
    so nothing that relied on the old semantics silently loosened."""
    root = _clone()
    subprocess.run([sys.executable, str(RECEIPT), "write", "--project", str(root)],
                   check=True, capture_output=True)
    assert _check(root) == 0
    with open(root / "scripts/lint.sh", "a") as fh:
        fh.write("\n# edit\n")
    assert _check(root) == 1


if __name__ == "__main__":
    fails = []
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"  PASS {name}")
            except Exception as exc:
                fails.append(name)
                print(f"  FAIL {name}: {exc}")
    sys.exit(1 if fails else 0)
