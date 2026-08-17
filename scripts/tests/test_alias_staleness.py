#!/usr/bin/env python3
"""Fixtures for the `alias-staleness` promotion check.

Every fixture here is one of the ways a name-based liveness scan gets the answer
WRONG, and each was found on the real tree before it was written down:

  * a reference that is only a COMMENT (the tree's `fence_step`, which grep
    called healthy three times over);
  * a name collision between a module function and a METHOD of the object it
    returns (`unclosed_reason`, which grep scored 22 live references for while
    no gate called the function at all);
  * a caller that lives inside an embedded shell heredoc, invisible to any
    Python-only closure (`scripts/lint.sh` calls four exports that way, and a
    `*.py`-only scan would have had them all deleted);
  * annotation-shaped signature difference that is not drift (the shim uses
    postponed annotations, so 8 of its 10 wrappers "differ" from their targets
    on an unchanged tree).

The DIRECTION of each failure is what matters. A false LIVE leaves a dead
wrapper in the tree, which is untidy; a false DEAD deletes working code. So the
soundness fixtures below all pin the second kind.

    python3 scripts/tests/test_alias_staleness.py
"""
from __future__ import annotations

import importlib.util
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
_FAILS = []
_ASSERTS = 0


def check(name, cond):
    global _ASSERTS
    _ASSERTS += 1
    if not cond:
        _FAILS.append(name)


def _load():
    src = REPO / "scripts/lint/check_alias_staleness.py"
    spec = importlib.util.spec_from_file_location("_cas", src)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


CAS = _load()

SHIM = '''"""fixture shim"""
from __future__ import annotations
import importlib.util, sys
from pathlib import Path

__all__ = ["alpha", "beta", "gamma", "local_only"]

_CS = None


def _cache_schema():
    global _CS
    if _CS is None:
        src = Path(__file__).resolve().parent / "todo-graph" / "cache_schema.py"
        spec = importlib.util.spec_from_file_location("_fx_cs", src)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _CS = mod
    return _CS


def alpha(lines):
    """delegating"""
    return _cache_schema().alpha(lines)


def beta(text: str):
    """delegating"""
    return _cache_schema().beta(text)


def gamma(x):
    """delegating"""
    return _cache_schema().gamma(x)


def local_only(a, b):
    """a real local implementation, not an alias"""
    return a + b
'''

TARGET = '''class Result:
    def gamma(self):
        """A METHOD sharing a name with the module function below."""
        return "method"


def alpha(lines):
    return Result()


def beta(text: str) -> str:
    return text


def gamma(x):
    return "function"
'''


def build_tree(root: pathlib.Path, consumers: dict[str, str], target: str = TARGET):
    (root / "scripts" / "todo-graph").mkdir(parents=True)
    (root / "scripts" / "todo_fence.py").write_text(SHIM)
    (root / "scripts" / "todo-graph" / "cache_schema.py").write_text(target)
    for rel, body in consumers.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(body)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True,
                   capture_output=True)
    return root


def verdicts(root: pathlib.Path):
    """`(dead, testonly, unknown, drift)` for a fixture tree."""
    names, live, testonly, unknown, drift = CAS.analyse(root)
    dead = {n for n in names if not live[n] and not testonly[n]}
    tonly = {n for n in names if not live[n] and testonly[n]}
    return dead, tonly, unknown, drift


def main() -> int:
    # ---- 1. a comment is NOT a caller -------------------------------------
    with tempfile.TemporaryDirectory() as d:
        root = build_tree(pathlib.Path(d), {
            "scripts/consumer.py":
                "import todo_fence as _f\n"
                "# _f.alpha is mentioned here only in a comment\n"
                "'''and _f.beta in a docstring'''\n"
                "def go():\n    return _f.gamma(1)\n",
        })
        dead, tonly, unknown, drift = verdicts(root)
        check("a commented reference does not keep an export alive",
              "alpha" in dead and "beta" in dead)
        check("a real call keeps its export alive", "gamma" not in dead)
        check("no spurious uncertainty on a simple tree", not unknown)

    # ---- 2. a METHOD of the returned object is a different symbol ---------
    with tempfile.TemporaryDirectory() as d:
        root = build_tree(pathlib.Path(d), {
            "scripts/consumer.py":
                "import todo_fence as _f\n"
                "def go(lines):\n"
                "    return _f.alpha(lines).gamma()\n",
        })
        dead, _, _, _ = verdicts(root)
        # THE CENTRAL TRAP. `.gamma()` here is a method on the object `alpha`
        # returned; crediting the shim's `gamma` for it is how the real tree
        # scored 22 live references for a wrapper no caller reaches.
        check("a method call does not credit the same-named module function",
              "gamma" in dead)
        check("the call that IS through the shim stays live", "alpha" not in dead)

    # ---- 3. a caller inside a shell heredoc still counts ------------------
    with tempfile.TemporaryDirectory() as d:
        root = build_tree(pathlib.Path(d), {
            "scripts/gate.sh":
                "#!/bin/bash\n"
                "python3 - <<'EOF'\n"
                "import importlib.util\n"
                "spec = importlib.util.spec_from_file_location('todo_fence', p)\n"
                "_fence = importlib.util.module_from_spec(spec)\n"
                "_fence.alpha(['x'])\n"
                "EOF\n",
        })
        dead, _, _, _ = verdicts(root)
        # Without the shell arm this is a false DEAD, and acting on it deletes
        # an export that a pre-commit gate calls on every commit.
        check("a heredoc caller keeps its export alive", "alpha" not in dead)

    # ---- 4. shell evidence must be QUALIFIED ------------------------------
    with tempfile.TemporaryDirectory() as d:
        root = build_tree(pathlib.Path(d), {
            "scripts/other.sh":
                "#!/bin/bash\n"
                "# todo_fence is named only in this comment\n"
                "python3 -c 'from cache_schema import alpha; alpha([])'\n",
        })
        dead, _, _, _ = verdicts(root)
        check("a shell file importing the TARGET does not credit the shim",
              "alpha" in dead)

    # ---- 5. binding shapes -------------------------------------------------
    with tempfile.TemporaryDirectory() as d:
        root = build_tree(pathlib.Path(d), {
            "scripts/byname.py":
                "import importlib.util\n"
                "from pathlib import Path\n"
                "def _fence():\n"
                "    spec = importlib.util.spec_from_file_location(\n"
                "        'todo_fence', Path('scripts/todo_fence.py'))\n"
                "    mod = importlib.util.module_from_spec(spec)\n"
                "    spec.loader.exec_module(mod)\n"
                "    return mod\n"
                "def go():\n"
                "    return _fence().alpha(['x'])\n",
            "scripts/late.py":
                "def early():\n"
                "    return _tf.beta('x')\n"
                "import todo_fence as _tf\n",
        })
        dead, _, unknown, _ = verdicts(root)
        # `loader().export` is the shape `todo-staged-check.py` uses, and a use
        # ABOVE its import is the shape the fence tests use; a single
        # source-order pass reported both of these DEAD.
        check("an export reached through a loader call is live", "alpha" not in dead)
        check("a use above its own import is live", "beta" not in dead)
        check("loader mechanics do not raise uncertainty", not unknown)

    # ---- 5b. a CACHED-GLOBAL loader, the shape that shipped a false DEAD ---
    with tempfile.TemporaryDirectory() as d:
        root = build_tree(pathlib.Path(d), {
            "hooks/triage.py":
                "import sys\n"
                "_fence = None\n"
                "def _load_fence():\n"
                "    global _fence\n"
                "    if _fence is None:\n"
                "        import todo_fence as _f\n"
                "        _fence = _f\n"
                "    return _fence\n"
                "def scan():\n"
                "    fence = _load_fence()\n"
                "    return fence.alpha(['x']), fence.beta('y')\n",
        })
        dead, _, unknown, _ = verdicts(root)
        # VERBATIM the shape in `.claude/hooks/sequencer_triage.py`: no
        # `spec_from_file_location` anywhere, the module cached in a global via a
        # plain name-to-name rebind, and handed back by a helper. Before the
        # binding fixpoint this credited ZERO referrers and raised no UNKNOWN, so
        # three live exports read DEAD with nothing to warn that the answer was
        # guessed.
        check("a cached-global loader keeps its exports alive",
              "alpha" not in dead and "beta" not in dead)
        check("the cached-global shape needs no uncertainty escape", not unknown)

    # ---- 6. genuinely opaque access suppresses DEAD ------------------------
    with tempfile.TemporaryDirectory() as d:
        root = build_tree(pathlib.Path(d), {
            "scripts/dyn.py":
                "import todo_fence as _f\n"
                "def go(name):\n"
                "    return getattr(_f, name)(1)\n",
        })
        dead, _, unknown, _ = verdicts(root)
        check("getattr on the shim is reported as uncertainty", bool(unknown))

    # ---- 7. test-only reachability is not liveness ------------------------
    with tempfile.TemporaryDirectory() as d:
        root = build_tree(pathlib.Path(d), {
            "scripts/tests/test_thing.py":
                "import todo_fence as _f\n"
                "def t():\n    return _f.alpha(['x'])\n",
        })
        dead, tonly, _, _ = verdicts(root)
        check("an export only its own test reaches is TEST-ONLY, not live",
              "alpha" in tonly and "alpha" not in dead)

    # ---- 8. annotation shape is NOT drift ---------------------------------
    with tempfile.TemporaryDirectory() as d:
        root = build_tree(pathlib.Path(d), {"scripts/c.py": "import todo_fence\n"})
        _, _, _, drift = verdicts(root)
        # The shim fixture carries `from __future__ import annotations` and its
        # target annotates a return the shim does not -- exactly the shape that
        # made a raw `inspect.signature` comparison reject 8 of 10 healthy
        # wrappers on the real tree.
        check("differing annotations alone are not reported as drift", not drift)

    # ---- 9. an added optional parameter IS drift --------------------------
    with tempfile.TemporaryDirectory() as d:
        drifted = TARGET.replace("def beta(text: str) -> str:",
                                 "def beta(text: str, extra=None) -> str:")
        root = build_tree(pathlib.Path(d), {"scripts/c.py": "import todo_fence\n"},
                          target=drifted)
        _, _, _, drift = verdicts(root)
        # The silent one: nothing raises, no test fails, and the new parameter
        # is simply unreachable through the fixed-arity wrapper forever.
        check("an optional parameter added to the target is drift",
              any("beta" in d_ for d_ in drift))

    # ---- 10. a CHANGED default is drift, not just an added parameter ------
    with tempfile.TemporaryDirectory() as d:
        drifted = TARGET.replace("def gamma(x):", "def gamma(x, flag=True):")
        root = build_tree(pathlib.Path(d), {"scripts/c.py": "import todo_fence\n"},
                          target=drifted)
        _, _, _, drift = verdicts(root)
        check("a target parameter the wrapper cannot pass is drift",
              any("gamma" in d_ for d_ in drift))

    with tempfile.TemporaryDirectory() as d:
        # BOTH sides carry the parameter, so arity and kind agree and only the
        # VALUE moved. Comparing defaults by presence alone passes this while
        # the wrapper goes on supplying the old one.
        shim2 = SHIM.replace("def gamma(x):\n    \"\"\"delegating\"\"\"\n    return _cache_schema().gamma(x)",
                             "def gamma(x, flag=True):\n    \"\"\"delegating\"\"\"\n    return _cache_schema().gamma(x, flag)")
        drifted = TARGET.replace("def gamma(x):", "def gamma(x, flag=False):")
        root = pathlib.Path(d)
        (root / "scripts" / "todo-graph").mkdir(parents=True)
        (root / "scripts" / "todo_fence.py").write_text(shim2)
        (root / "scripts" / "todo-graph" / "cache_schema.py").write_text(drifted)
        (root / "scripts" / "c.py").write_text("import todo_fence\n")
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        subprocess.run(["git", "-C", str(root), "add", "-A"], check=True,
                       capture_output=True)
        _, _, _, drift = verdicts(root)
        check("a changed default VALUE is drift", any("gamma" in d_ for d_ in drift))

    # ---- 11. main() ITSELF, exit code and all --------------------------
    # THE GAP THAT SHIPPED A DEFECT. Every fixture above calls `verdicts()`,
    # which goes straight to `analyse()` -- so none of them exercised main()'s
    # suppression rule or its exit code, and the checker spent a round silently
    # unable to fail: once this very file was tracked, a fixture string
    # mentioning `todo_fence` bound a local as the shim, raised an UNKNOWN, and
    # UNKNOWN suppresses DEAD. It printed one line and exited 0.
    with tempfile.TemporaryDirectory() as d:
        root = build_tree(pathlib.Path(d), {
            "scripts/consumer.py":
                "import todo_fence as _f\n"
                "def go():\n    return _f.gamma(1)\n",
        })
        rc = CAS.main(["check_alias_staleness", str(root)])
        check("main() exits nonzero when an export is DEAD", rc == 1)

    with tempfile.TemporaryDirectory() as d:
        root = build_tree(pathlib.Path(d), {
            "scripts/consumer.py":
                "import todo_fence as _f\n"
                "def go():\n"
                "    return (_f.alpha(1), _f.beta('x'), _f.gamma(2),\n"
                "            _f.local_only(1, 2))\n",
        })
        rc = CAS.main(["check_alias_staleness", str(root)])
        check("main() exits 0 when every export is reached", rc == 0)

    with tempfile.TemporaryDirectory() as d:
        # A fixture-shaped call carrying the module NAME deep in a nested
        # argument must not bind its result as the shim.
        root = build_tree(pathlib.Path(d), {
            "scripts/consumer.py":
                "import todo_fence as _f\n"
                "import subprocess\n"
                "def helper(a, b):\n    return a\n"
                "def go():\n"
                "    root = helper('/tmp', {'x': 'load todo_fence here'})\n"
                "    subprocess.run([str(root)])\n"
                "    return _f.alpha(1), _f.beta('x'), _f.gamma(2), _f.local_only(1, 2)\n",
        })
        _, _, unknown, _ = verdicts(root)
        check("a nested module-name literal does not bind an unrelated local",
              not unknown)

    # ---- 12. dynamic consumers must never read DEAD ---------------------
    # EACH OF THESE WAS A LIVE FALSE-DEAD, found by review after the check had
    # already been promoted to a build-blocking error. A consumer that plainly
    # uses every export returned dead=[all of them] with nothing flagged.
    for label, body in (
        ("dict container",
         "import todo_fence as tf\n"
         "registry = {'fence': tf}\n"
         "def go():\n    return registry['fence'].alpha([])\n"),
        ("keyword escape",
         "import todo_fence as tf\n"
         "def helper(**kw):\n    return kw\n"
         "def go():\n    return helper(mod=tf)\n"),
    ):
        with tempfile.TemporaryDirectory() as d:
            root = build_tree(pathlib.Path(d), {"scripts/c.py": body})
            dead, _, unknown, _ = verdicts(root)
            check(f"{label}: uncertainty is raised rather than a DEAD verdict",
                  bool(unknown))
            check(f"{label}: main() does not report a blocking failure",
                  CAS.main(["x", str(root)]) != 2)

    with tempfile.TemporaryDirectory() as d:
        # `from scripts import todo_fence as tf` -- the module arrives as a NAME
        # in the import list rather than as the module imported FROM.
        root = build_tree(pathlib.Path(d), {
            "scripts/c.py":
                "from scripts import todo_fence as tf\n"
                "def go():\n"
                "    return tf.alpha(1), tf.beta('x'), tf.gamma(2), tf.local_only(1, 2)\n",
        })
        dead, _, unknown, _ = verdicts(root)
        check("a package-qualified import is resolved, not guessed at",
              not dead and not unknown)

    # ---- 13. the exit code is TYPED, and the two halves differ -----------
    with tempfile.TemporaryDirectory() as d:
        drifted = TARGET.replace("def gamma(x):", "def gamma(x, flag=True):")
        root = build_tree(pathlib.Path(d), {
            "scripts/c.py":
                "import todo_fence as tf\n"
                "def go():\n"
                "    return tf.alpha(1), tf.beta('x'), tf.gamma(2), tf.local_only(1, 2)\n",
        }, target=drifted)
        check("DRIFT reports advisory, not blocking",
              CAS.main(["x", str(root)]) == 1)

    with tempfile.TemporaryDirectory() as d:
        root = build_tree(pathlib.Path(d), {
            "scripts/c.py":
                "import todo_fence as tf\n"
                "def go():\n    return tf.gamma(1)\n",
        })
        # DEAD is advisory: it rests on a reference scan, and blocking a commit
        # on a heuristic is what the round-3 consumers showed to be wrong.
        check("DEAD alone exits 1, the advisory code",
              CAS.main(["x", str(root)]) == 1)

    # ---- 14. escapes this check spent four rounds failing to see ---------
    for label, files in (
        ("cross-module attribute call", {
            "scripts/c.py":
                "import todo_fence as tf\n"
                "import helper\n"
                "def go():\n    return helper.use(tf)\n",
            "scripts/helper.py": "def use(mod):\n    return mod.alpha([])\n"}),
        ("attribute store on an object", {
            "scripts/c.py":
                "import todo_fence as tf\n"
                "class Box:\n    pass\n"
                "def go():\n"
                "    b = Box()\n"
                "    b.fence = tf\n"
                "    return b.fence.alpha([])\n"}),
    ):
        with tempfile.TemporaryDirectory() as d:
            root = build_tree(pathlib.Path(d), files)
            dead, _, unknown, _ = verdicts(root)
            # The receiving module cannot know its parameter is the shim, and an
            # object attribute is not a name this walker resolves -- so the only
            # honest answer is uncertainty, never DEAD.
            check(f"{label}: raises uncertainty instead of a DEAD verdict",
                  bool(unknown))

    with tempfile.TemporaryDirectory() as d:
        root = build_tree(pathlib.Path(d), {
            "scripts/c.py":
                "from scripts.todo_fence import alpha, beta, gamma, local_only\n"
                "def go():\n    return alpha([]), beta('x'), gamma(1), local_only(1, 2)\n",
        })
        dead, _, unknown, _ = verdicts(root)
        check("a package-qualified DIRECT import is a resolved reference",
              not dead and not unknown)

    # ---- 15. UNKNOWN must suppress the OUTPUT, not merely the exit code ---
    with tempfile.TemporaryDirectory() as d:
        root = build_tree(pathlib.Path(d), {
            "scripts/c.py":
                "import todo_fence as tf\n"
                "registry = {'f': tf}\n"
                "def go():\n    return registry['f'].alpha([])\n",
        })
        import io, contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            CAS.main(["x", str(root)])
        out = buf.getvalue()
        # Printing "DEAD: beta" beside "reachability is unknown" is what invites
        # the unsafe retirement the suppression rule exists to prevent, and the
        # verdicts used to be computed and printed before `unknown` was read.
        check("an UNKNOWN run prints no DEAD verdict", "DEAD:" not in out)
        check("an UNKNOWN run prints no TEST-ONLY verdict", "TEST-ONLY:" not in out)
        check("an UNKNOWN run says so", "UNKNOWN" in out)

    # ---- 16. a checker that CANNOT RUN exits 3, never the advisory code --
    CHECKER = str(REPO / "scripts/lint/check_alias_staleness.py")
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / "scripts").mkdir(parents=True)
        (root / "scripts" / "todo_fence.py").write_text("__all__ = [\n")  # unparseable
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        subprocess.run(["git", "-C", str(root), "add", "-A"], check=True,
                       capture_output=True)
        r = subprocess.run([sys.executable, CHECKER, str(root)],
                           capture_output=True, text=True)
        # THE COLLISION THIS PINS: a helper that raised SystemExit exited 1,
        # which is the ADVISORY code, so lint rendered a checker that analysed
        # nothing as an ordinary warning and the check silently stopped existing.
        check("an unparseable shim exits 3, the reserved fatal code", r.returncode == 3)
        check("and says FATAL so lint can classify it", "FATAL" in r.stdout)

    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / "scripts").mkdir(parents=True)
        (root / "scripts" / "todo_fence.py").write_text("__all__ = [x]\nx = 1\n")
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        subprocess.run(["git", "-C", str(root), "add", "-A"], check=True,
                       capture_output=True)
        r = subprocess.run([sys.executable, CHECKER, str(root)],
                           capture_output=True, text=True)
        check("a non-literal __all__ exits 3 rather than reading as advisory",
              r.returncode == 3 and "FATAL" in r.stdout)

    with tempfile.TemporaryDirectory() as d:
        # An unparseable CONSUMER, which is a different code path from an
        # unparseable shim: this one goes through `scan_python`, the fail-closed
        # branch whose whole point is that a file it cannot read is not a file
        # with no callers in it.
        root = build_tree(pathlib.Path(d), {"scripts/broken.py": "def (:\n"})
        r = subprocess.run([sys.executable, CHECKER, str(root)],
                           capture_output=True, text=True)
        # ADVISORY, not fatal. The scan reads the working tree while a
        # pre-commit lint judges the index, so an unrelated file left
        # temporarily invalid in the worktree is not in the commit -- making it
        # fatal blocked commits that did not contain the syntax error.
        check("an unparseable CONSUMER is uncertainty, not a malfunction",
              r.returncode == 1 and "FATAL" not in r.stdout)
        check("and it suppresses DEAD rather than being skipped",
              "DEAD:" not in r.stdout and "UNKNOWN" in r.stdout)

    # ---- 17. the last two false-DEAD shapes -----------------------------
    with tempfile.TemporaryDirectory() as d:
        # A typed alias. `tf: ModuleType = raw` is ordinary Python, and walking
        # only single-name Assign nodes returned no hit AND no uncertainty.
        root = build_tree(pathlib.Path(d), {
            "scripts/c.py":
                "from types import ModuleType\n"
                "import todo_fence as raw\n"
                "tf: ModuleType = raw\n"
                "def go():\n"
                "    return tf.alpha(1), tf.beta('x'), tf.gamma(2), tf.local_only(1, 2)\n",
        })
        dead, _, unknown, _ = verdicts(root)
        check("a typed module alias is resolved, not read as absence",
              not dead and not unknown)

    with tempfile.TemporaryDirectory() as d:
        # A heredoc importing the exports DIRECTLY: no module prefix for the
        # attribute pattern to find, so the shell arm saw nothing at all.
        root = build_tree(pathlib.Path(d), {
            "scripts/gate.sh":
                "#!/bin/bash\n"
                "python3 - <<'EOF'\n"
                "from todo_fence import alpha, beta, gamma, local_only\n"
                "alpha([]); beta('x'); gamma(1); local_only(1, 2)\n"
                "EOF\n",
        })
        dead, _, _, _ = verdicts(root)
        check("a heredoc DIRECT import keeps its exports alive", not dead)

    with tempfile.TemporaryDirectory() as d:
        # A heredoc that loads the shim but whose calls this scan cannot match
        # must say so rather than report absence.
        root = build_tree(pathlib.Path(d), {
            "scripts/gate.sh":
                "#!/bin/bash\n"
                "python3 - <<'EOF'\n"
                "import todo_fence\n"
                "fn = getattr(todo_fence, name)\n"
                "fn([])\n"
                "EOF\n",
        })
        dead, _, unknown, _ = verdicts(root)
        check("an unresolvable heredoc raises uncertainty, not DEAD", bool(unknown))

    with tempfile.TemporaryDirectory() as d:
        # And a shell file that merely NAMES the shim (copying the file, or
        # registering a test whose filename contains it) is not a consumer --
        # treating it as one raised a permanent UNKNOWN that suppressed DEAD.
        root = build_tree(pathlib.Path(d), {
            "scripts/other.sh":
                "#!/bin/bash\n"
                "cp scripts/todo_fence.py \"$1/scripts/todo_fence.py\"\n"
                "python3 scripts/tests/test_todo_fence.py\n",
            "scripts/c.py":
                "import todo_fence as tf\n"
                "def go():\n"
                "    return tf.alpha(1), tf.beta('x'), tf.gamma(2), tf.local_only(1, 2)\n",
        })
        dead, _, unknown, _ = verdicts(root)
        check("naming the shim file is not consuming it", not unknown and not dead)

    if _FAILS:
        for f in _FAILS:
            print(f"FAIL: {f}")
        print(f"test_alias_staleness FAILED ({len(_FAILS)} failing)")
        return 1
    print(f"test_alias_staleness OK ({_ASSERTS} assertions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
