#!/usr/bin/env python3
# Live wedge, 2026-07-28 03:42 (attended canary). The run refused a verified
# rollover with "blocked by the operator's in-flight TODO-25 edit, which isn't
# mine to clean". No operator had touched TODO-25: the dirty content was four
# XREF line-number bumps (467->468, 302->303, 463->464, 464->465), all pointing
# into TODO-21 -- the file the run had just edited, shifting those very lines.
# The run's OWN tooling (stranded_deferrals.py --owner, todo-graph
# build-and-validate.sh) wrote them minutes earlier.
#
# The misattribution is SELF-SUSTAINING: every section that shifts a
# cross-referenced line re-dirties a TODO the run refuses to own, so rollover
# never fires without a human committing for it (operator commit 0f4d6995 was
# needed to unblock). run_phase_guard now classifies by CONTENT.
#
# The property under test cuts BOTH ways and the blocking half matters more: a
# diff that is anything other than pure XREF line-number repair must still
# block, or this tolerance becomes a hole that rotates away real work.
import importlib.util
import pathlib
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
HOOK = HERE.parents[2] / ".claude/hooks/run_phase_guard.py"
FAILS = []

# Verbatim from commit 0f4d6995 -- the exact line whose bump wedged the run.
REAL = ('> **Accepted:** [M] `ob_job_create` inserts a named job into the object '
        'namespace before allocating its handle, so a handle-alloc failure leaks '
        'the directory entry, the body, and now its quota block (reason: scope, '
        'pre-existing Job-Object lifecycle) -> XREF: `02-kernel-core/TODO-21 '
        'S14` (item: "`ob_job_create` inserts a named job into '
        '`\\BaseNamedObjects`" at line {n})\n')


def check(name, cond):
    if not cond:
        FAILS.append(name)


def _load():
    spec = importlib.util.spec_from_file_location("rpg_xref", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _git(root, *args):
    subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True)


def _repo(d):
    root = pathlib.Path(d)
    _git(root, "init", "-q")
    _git(root, "config", "user.email", "t@t")
    _git(root, "config", "user.name", "t")
    (root / "todo").mkdir()
    (root / "todo" / "TODO-25.md").write_text(REAL.format(n=467))
    _git(root, "add", "-A")
    _git(root, "-c", "commit.gpgsign=false", "commit", "-q", "-m", "seed")
    return root


def test_the_real_wedge_is_tolerated():
    with tempfile.TemporaryDirectory() as d:
        mod, root = _load(), _repo(d)
        (root / "todo" / "TODO-25.md").write_text(REAL.format(n=468))
        check("real bump classified xref-repair",
              mod._dirty_owner(" M todo/TODO-25.md", root) == "xref-repair")
        check("classifier agrees",
              mod._is_xref_lineno_repair(root, "todo/TODO-25.md") is True)
        check("xref-repair is a tolerated owner",
              "xref-repair" in mod._TOLERATED_OWNERS)


def test_anything_more_than_a_number_still_blocks():
    """The load-bearing half: tolerance must not swallow real work."""
    with tempfile.TemporaryDirectory() as d:
        mod, root = _load(), _repo(d)
        p = root / "todo" / "TODO-25.md"

        p.write_text(REAL.format(n=468).replace("quota block", "QUOTA BLOCK"))
        check("prose edit alongside a bump blocks",
              mod._dirty_owner(" M todo/TODO-25.md", root) == "tracked-source")

        p.write_text(REAL.format(n=467) + "- [ ] brand new item\n")
        check("an added line blocks",
              mod._dirty_owner(" M todo/TODO-25.md", root) == "tracked-source")

        p.write_text(REAL.format(n=468).replace("XREF:", "SEEALSO:"))
        check("a non-XREF line-number change blocks",
              mod._dirty_owner(" M todo/TODO-25.md", root) == "tracked-source")

        # STAGED is never tolerated, on any path -- it is uncommitted index state.
        p.write_text(REAL.format(n=468))
        _git(root, "add", "todo/TODO-25.md")
        check("a STAGED bump blocks",
              mod._dirty_owner("M  todo/TODO-25.md", root) == "tracked-source")
        _git(root, "reset", "-q")

        check("a non-todo path blocks",
              mod._dirty_owner(" M src/kernel/x.c", root) == "tracked-source")
        check("a deleted todo blocks",
              mod._dirty_owner(" D todo/TODO-25.md", root) == "tracked-source")


def test_backward_compatible_and_fail_closed():
    with tempfile.TemporaryDirectory() as d:
        mod, root = _load(), _repo(d)
        (root / "todo" / "TODO-25.md").write_text(REAL.format(n=468))
        # No root -> content check skipped, behaviour identical to before.
        check("no-root keeps prior behaviour",
              mod._dirty_owner(" M todo/TODO-25.md") == "tracked-source")
        # The pre-existing auto-gen allowlist is untouched.
        check("auto-gen allowlist still tolerated",
              mod._dirty_owner(" M COUNT.md", root) == "auto-gen")
        # Unsure -> keep blocking (a path git cannot diff must not be tolerated).
        check("missing path fails closed",
              mod._is_xref_lineno_repair(root, "todo/does-not-exist.md") is False)
        check("non-repo root fails closed",
              mod._is_xref_lineno_repair(pathlib.Path("/nonexistent-xyz"),
                                         "todo/TODO-25.md") is False)


if __name__ == "__main__":
    test_the_real_wedge_is_tolerated()
    test_anything_more_than_a_number_still_blocks()
    test_backward_compatible_and_fail_closed()
    if FAILS:
        for f in FAILS:
            print("FAIL:", f)
        raise SystemExit(1)
    print("test_rollover_xref_repair OK (wedge tolerated, real work still blocks)")
