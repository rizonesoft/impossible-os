#!/usr/bin/env python3
"""Report an alias in `scripts/todo_fence.py` that has outlived its reason.

WHAT THE CLASS IS. `todo_fence` is a shim: most of its exports are one-line
wrappers forwarding to `scripts/todo-graph/cache_schema.py`. Sections 36, 38,
41, 42, 43 and 44 each retired a private parser to one of these wrappers, so the
alias population is large and still growing. `alias-staleness` is the defect
shape that leaves behind: a wrapper whose last caller went away, or a wrapper
that quietly stopped matching what it forwards to. The finding-ledger put the
class past the promotion threshold at 4 fixed findings; this IS that promotion.
It REPORTS rather than refuses: every verdict is advisory and only a checker that
could not run is an error, because four review rounds produced live consumers and
behaviour-preserving refactors that a blocking version would have stopped.

WHY A TEXTUAL SCAN IS THE WRONG TOOL, measured on this tree. `grep` counts 3
references to `fence_step` and 22 to `unclosed_reason`, and BOTH answers are
wrong in the dangerous direction:

  * every `fence_step` reference is a COMMENT, so the wrapper is dead while a
    text scan calls it healthy;
  * every production `unclosed_reason` reference is the `ScanResult` METHOD
    (`scan.unclosed_reason()`), not this module's free function -- the two
    symbols share a name and nothing else.

So references are resolved through the AST, and only a reference reaching a
name the file PROVES is the shim module counts as use.

THE THREE VERDICTS, and why there is no fourth. A name-based scan cannot be
sound in both directions at once, so the classes stay apart:

  * LIVE       -- a resolved reference through a shim-bound name, non-test file.
  * TEST-ONLY  -- resolved, but every referrer is a test. A test exercising an
                  export is not a reason for the export to exist; saying so is
                  the entire point of separating this from LIVE.
  * UNKNOWN    -- the file does something this checker cannot follow (a star
                  import, `getattr` on the shim, a shim-bound name escaping into
                  another call). UNKNOWN suppresses the DEAD verdict for every
                  export, because the alternative is deleting live code.

DEAD is reported only when a name has zero LIVE and zero TEST-ONLY references
AND nothing anywhere came back UNKNOWN. Absence of evidence is read as evidence
of absence in exactly one place -- a tree with no unfollowable construct in it.

SHELL IS LIVE-ONLY EVIDENCE, deliberately asymmetric. `scripts/lint.sh` calls
four exports from inside an embedded Python heredoc, so a Python-only closure
would call `staged_docs`, `StagedSnapshotError`, `scan_text` and `mask_text`
dead and invite deleting them. A heredoc cannot be resolved the way a module
can, so a shell hit counts only FOR liveness, never against it, and only in a
file that names `todo_fence` at all.

THE CLOSURE IS EVERY TRACKED `.py`/`.sh` FILE, not a directory whitelist. An
earlier design scanned `scripts/` and `.claude/hooks/`; 37 tracked Python and
shell files live outside those two, nothing stops one of them loading the shim
by path, and such a caller would have been invisible with its export reported
DEAD. The closure STOPS at those two extensions, honestly: a consumer written
in an extensionless script or another tracked format is outside what this
checker enumerates at all, produces no UNKNOWN, and is a residual false-DEAD
risk no different in kind from the undecidable-reachability residual the
section already records -- Python's own consumer population is what the class
was mined from, and widening the file-type scan is future work, not a promise
made here.
"""
from __future__ import annotations

import ast
import inspect
import re
import subprocess
import sys
from pathlib import Path

SHIM_REL = "scripts/todo_fence.py"


class CheckerFailed(Exception):
    """The check could not RUN. Distinct from any verdict it might produce.

    `SystemExit` was the obvious spelling and it is wrong here: Python exits 1
    for it, which is this tool's ADVISORY code, so a git failure or an
    unparseable file rendered as an ordinary warning and the check reported
    nothing wrong while having analysed nothing.
    """


class NotAGitRepo(Exception):
    """`root` is not inside a git worktree -- NOT a checker malfunction.

    `scripts/test-tooling.sh` runs `lint.sh` against dozens of scratch
    directories that are never `git init`-ed (they only need a couple of files
    on disk, not commit history), and this checker's whole notion of "the
    tracked tree" is git-shaped. Treating a bare directory as FATAL made Check
    28 abort the ENTIRE lint run in every one of those sandboxes -- a real
    regression, caught by the pre-push tooling gate rather than a hand review.
    """


def _in_git_worktree(root: Path) -> bool:
    """True iff `root` or an ancestor carries a `.git` entry.

    A FILESYSTEM check, not a git-stderr check. Matching on stderr TEXT was
    tried and is wrong two ways at once: it is locale-dependent (a non-English
    git produces a different message, missing the match and recreating the
    FATAL-in-a-sandbox regression this replaces), and it is over-broad (`git
    -C root ... ` with `GIT_DIR` pointed elsewhere prints the SAME "not a git
    repository" wording against a `root` that genuinely IS a valid repo,
    silently disarming the fail-closed contract). Checking the filesystem
    directly depends on neither. A `.git` FILE, not just a directory, covers a
    submodule.
    """
    cur = root
    while True:
        if (cur / ".git").exists():
            return True
        if cur.parent == cur:
            return False
        cur = cur.parent


def _tracked(root: Path) -> list[str]:
    """Every tracked `.py`/`.sh` path, so the closure cannot silently narrow."""
    if not _in_git_worktree(root):
        raise NotAGitRepo(str(root))
    r = subprocess.run(["git", "-C", str(root), "ls-files", "*.py", "*.sh"],
                       capture_output=True, text=True, check=False)
    if r.returncode != 0:
        # A `.git` entry exists but `ls-files` still failed -- corrupt tree,
        # permissions, an unsupported git version. Unconditionally FATAL; the
        # "ordinary bare directory" case was already ruled out above.
        raise CheckerFailed(f"git ls-files failed: {r.stderr.strip()[:200]}")
    return [p for p in r.stdout.split("\n") if p]


def _is_test(rel: str) -> bool:
    name = rel.rsplit("/", 1)[-1]
    return ("/tests/" in rel or name.startswith("test_")
            or name.startswith("test-") or name.endswith("_test.py"))


def shim_exports(shim: Path) -> tuple[list[str], dict[str, str]]:
    """`(__all__, {export: forwarded cache_schema attribute})`.

    The second map is what makes the signature check possible: only a wrapper
    whose body is a single `return _cache_schema().NAME(...)` is a delegating
    alias with a target to compare against. A local implementation is not an
    alias and is deliberately absent from the map.
    """
    tree = ast.parse(shim.read_text(encoding="utf-8"), filename=str(shim))
    names: list[str] = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == "__all__" for t in node.targets):
            if not isinstance(node.value, (ast.List, ast.Tuple)):
                raise CheckerFailed("__all__ is not a literal")
            for elt in node.value.elts:
                if not isinstance(elt, ast.Constant) or not isinstance(elt.value, str):
                    raise CheckerFailed("__all__ has a non-literal")
                names.append(elt.value)
    if not names:
        raise CheckerFailed("no __all__ found in the shim")

    forwards: dict[str, str] = {}
    for node in tree.body:
        if not isinstance(node, ast.FunctionDef) or node.name not in names:
            continue
        body = [s for s in node.body if not (isinstance(s, ast.Expr)
                                             and isinstance(s.value, ast.Constant))]
        if len(body) != 1 or not isinstance(body[0], ast.Return):
            continue
        call = body[0].value
        if not isinstance(call, ast.Call) or not isinstance(call.func, ast.Attribute):
            continue
        inner = call.func.value
        if (isinstance(inner, ast.Call) and isinstance(inner.func, ast.Name)
                and inner.func.id == "_cache_schema"):
            forwards[node.name] = call.func.attr
    return names, forwards


def _loads_shim(node: ast.AST) -> bool:
    """True iff this subtree builds a module spec that NAMES `todo_fence`.

    The discriminator has to be the spec, not the loader call. This repo loads a
    great many modules by path -- `run_phase_guard`, `section-manifest`,
    `utest-frame-difftest` and dozens of test fixtures all do the same
    `spec_from_file_location` / `module_from_spec` dance -- so treating any
    `module_from_spec(...)` as a shim binding marks every one of those variables
    as the shim. The first cut of this checker did exactly that and produced
    ~190 bogus UNKNOWN lines, which then suppressed the DEAD verdicts the check
    exists to make. A docstring mention is not enough either: several of these
    files name `todo_fence` in prose while loading something else.
    """
    for n in ast.walk(node):
        if not isinstance(n, ast.Call):
            continue
        f = n.func
        fname = f.attr if isinstance(f, ast.Attribute) else \
            (f.id if isinstance(f, ast.Name) else "")
        if fname != "spec_from_file_location":
            continue
        for a in list(n.args) + [k.value for k in n.keywords]:
            if isinstance(a, ast.Constant) and isinstance(a.value, str) \
                    and "todo_fence" in a.value:
                return True
            # `... / "todo_fence.py"` reaches here as the right operand.
            for sub in ast.walk(a):
                if isinstance(sub, ast.Constant) and isinstance(sub.value, str) \
                        and "todo_fence" in sub.value:
                    return True
    return False


def _loader_functions(tree: ast.AST) -> set[str]:
    """Functions that load the shim by path and hand back the module.

    Two consumers wrap the by-path import in a helper (`todo-staged-check.py`,
    `todo-orphan-check.py`), so the module arrives through a call rather than an
    assignment this walker could otherwise follow.
    """
    return {n.name for n in ast.walk(tree)
            if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
            and _loads_shim(n)}


def _call_name(f: ast.AST) -> str:
    return f.attr if isinstance(f, ast.Attribute) else \
        (f.id if isinstance(f, ast.Name) else "")


def _shim_bound_names(tree: ast.AST, loaders: set[str],
                      top_level_only: bool = False,
                      seed: set[str] | None = None) -> tuple[set[str], list[str]]:
    """Local names this file PROVES are the shim module, plus star-imports.

    A SEPARATE PASS, and that is not tidiness. A single walk collects bindings
    and uses in source order, so a use above its binding is invisible: in
    `scripts/tests/test_todo_fence.py` the `tf.unmasked(...)` call sits at line
    181 while one of the `tf` bindings is at line 1288, and the one-pass version
    of this checker therefore reported `unmasked` DEAD -- a false DEAD, which is
    the one direction this check must never be wrong in.

    Three binding shapes exist in the tree and all are followed: `import
    todo_fence as X`; the by-path `spec_from_file_location("todo_fence", ...)`
    dance whose module lands in a variable; and a helper (whether it hardcodes
    the shim, like `todo-staged-check._fence`, or takes it as an argument, like
    the tests' generic `load("todo_fence", ...)`).
    """
    bound: set[str] = set(seed or ())
    specs: set[str] = set()
    unknown: list[str] = []

    def _names_shim(v: ast.Call) -> bool:
        for a in list(v.args) + [k.value for k in v.keywords]:
            for sub in ast.walk(a):
                if isinstance(sub, ast.Constant) and isinstance(sub.value, str) \
                        and "todo_fence" in sub.value:
                    return True
        return False

    def _fname(f: ast.AST) -> str:
        return f.attr if isinstance(f, ast.Attribute) else \
            (f.id if isinstance(f, ast.Name) else "")

    def yields_shim(v: ast.AST) -> bool:
        if not isinstance(v, ast.Call):
            return False
        name = _fname(v.func)
        # A SPEC IS NOT A MODULE. `spec_from_file_location("todo_fence", ...)`
        # names the shim but produces a spec object; binding it as the module
        # made every `module_from_spec(spec)` look like the shim being handed to
        # a call, which is an UNKNOWN, and UNKNOWN suppresses DEAD -- so the
        # mistake quietly disarmed the check it belongs to.
        if name == "spec_from_file_location":
            return False
        if isinstance(v.func, ast.Name) and v.func.id in loaders:
            return True
        # A generic loader named at the CALL site: `load("todo_fence", path)`.
        # THE FIRST ARGUMENT ONLY, and exactly the module name. Searching every
        # nested constant instead made `root = build_tree(dir, {...fixtures...})`
        # bind `root` as the shim in this check's OWN test file, purely because
        # the fixture strings mention `todo_fence` -- which raised an UNKNOWN,
        # and an UNKNOWN suppresses DEAD, so the check silently stopped being
        # able to fail at all the moment its tests were tracked.
        if name != "module_from_spec" and v.args:
            a0 = v.args[0]
            if isinstance(a0, ast.Constant) and a0.value == "todo_fence":
                return True
        return name == "module_from_spec" and (
            _loads_shim(v) or any(isinstance(a, ast.Name) and a.id in specs
                                  for a in v.args))

    nodes = list(tree.body) if top_level_only else list(ast.walk(tree))

    for node in nodes:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 \
                and isinstance(node.targets[0], ast.Name) \
                and isinstance(node.value, ast.Call) \
                and _fname(node.value.func) == "spec_from_file_location" \
                and _names_shim(node.value):
            specs.add(node.targets[0].id)

    for node in nodes:
        if isinstance(node, ast.Import):
            for a in node.names:
                # `import scripts.todo_fence as tf` binds the alias; a bare
                # `import scripts.todo_fence` binds the PACKAGE root, and an
                # export is then reached as `scripts.todo_fence.NAME`, which
                # this walker does not resolve -- so it is uncertainty, not
                # absence.
                if a.name == "todo_fence" or a.name.endswith(".todo_fence"):
                    if a.asname:
                        bound.add(a.asname)
                    elif a.name == "todo_fence":
                        bound.add(a.name)
                    else:
                        unknown.append(f"package-qualified import of {a.name}")
        elif isinstance(node, ast.ImportFrom) and (
                node.module == "todo_fence"
                or (node.module or "").endswith(".todo_fence")):
            if any(a.name == "*" for a in node.names):
                unknown.append("star-import of todo_fence")
        elif isinstance(node, ast.ImportFrom) and any(
                a.name == "todo_fence" for a in node.names):
            # `from scripts import todo_fence as tf` -- the MODULE arrives as a
            # name in the import list, not as the module being imported from.
            for a in node.names:
                if a.name == "todo_fence":
                    bound.add(a.asname or a.name)
        elif (isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name)
              and node.value is not None) or (
                isinstance(node, ast.Assign) and len(node.targets) == 1
                and isinstance(node.targets[0], ast.Name)):
            # AnnAssign included: `tf: ModuleType = raw` is an ordinary typed
            # alias, and skipping it meant a consumer that plainly calls the
            # exports produced neither a hit nor uncertainty -- a false DEAD.
            tgt = node.target if isinstance(node, ast.AnnAssign) else node.targets[0]
            v = node.value
            # `fence = fence or _fence()` (todo-orphan-check.py:127) is the
            # default-an-injected-module idiom, so the shim-yielding call is one
            # level down inside a BoolOp or a conditional rather than being the
            # assigned value itself.
            cands = [v]
            if isinstance(v, ast.BoolOp):
                cands += list(v.values)
            elif isinstance(v, ast.IfExp):
                cands += [v.body, v.orelse]
            if any(yields_shim(c) for c in cands) \
                    or any(isinstance(c, ast.Name) and c.id in bound for c in cands):
                bound.add(tgt.id)
    return bound, unknown


def _mentions_shim(text: str) -> bool:
    """True if `text` could name `todo_fence` under ANY binding this scanner
    resolves -- the cheap pre-parse gate that skips `ast.parse` on a file that
    cannot possibly be a consumer.

    NOT a bare substring check. Python folds adjacent string literals at parse
    time -- `"todo_" "fence"` and `"todo_fence"` are the SAME AST constant --
    so a contiguous-token test misses a loader spec written that way and would
    skip the file before ever reaching the parse that would have resolved it.
    A regex tolerating whitespace/quotes/concatenation between the two halves
    costs a few more false CANDIDATES (an extra `ast.parse` each), which is the
    safe direction to be wrong in; a false SKIP costs a false DEAD.
    """
    if "todo_fence" in text:
        return True
    gap = "['\"\\s+]*"
    return re.search("todo_" + gap + "fence", text) is not None


def scan_python(path: Path, rel: str, exports: set[str]) -> tuple[set[str], list[str]]:
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        return set(), [f"cannot read {rel}: {exc}"]
    # SKIP THE PARSE for a file that cannot possibly reference the shim. No
    # binding shape this scanner resolves omits the literal `todo_fence`
    # somewhere, so a file without it cannot hide a reference under ANY of the
    # names this checker knows how to follow. Measured on the tracked tree:
    # 271 Python files, 13 contain the token, and `ast.parse`-ing all 271
    # anyway cost ~3.7 of the checker's ~4s. This also closes most of the
    # "tracked tree" overstatement below -- a candidate that never mentions the
    # module cannot be a consumer under this checker's own resolution rules.
    if not _mentions_shim(text):
        return set(), []
    try:
        tree = ast.parse(text, filename=rel)
    except SyntaxError as exc:
        # UNCERTAINTY, not a malfunction, and the distinction is load-bearing at
        # commit time. This scan reads the WORKING TREE while a pre-commit lint
        # judges the INDEX, so an unrelated file left temporarily invalid in the
        # worktree is not part of the commit at all -- treating it as fatal
        # blocked a commit that did not contain the syntax error. It is still
        # fail-closed: an unreadable file suppresses every DEAD verdict rather
        # than being silently skipped.
        return set(), [f"cannot parse {rel}: {exc}"]

    loaders = _loader_functions(tree)
    hits: set[str] = set()
    unknown: list[str] = []

    # PER SCOPE, not per file. A flat name set conflates same-named locals in
    # different functions, and this tree reuses `spec`/`mod` in a dozen by-path
    # loaders: `todo-staged-check.py` binds the shim as `mod` at line 63 and a
    # completely different module as `mod` at line 377, so the flat version
    # credited the shim with the second one and emitted an UNKNOWN that
    # suppressed every DEAD verdict in the run.
    funcs = {n.name: n for n in ast.walk(tree)
             if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))}
    scopes = [tree] + list(funcs.values())

    # LOADERS AND MODULE GLOBALS TO A FIXPOINT. `.claude/hooks/sequencer_triage.py`
    # caches the shim in a module global -- `import todo_fence as _f; _fence = _f;
    # return _fence` -- with no `spec_from_file_location` anywhere, so the
    # spec-shaped loader test never recognised `_load_fence()` and its three
    # callers vanished. Measured before this loop: scan_text, classify_heading and
    # is_h2 were each credited to ZERO referrers in that file, with no UNKNOWN to
    # show for it, which is the false DEAD that deletes working control-plane code.
    module_bound: set[str] = set()
    for _ in range(8):
        before = (len(loaders), len(module_bound))
        module_bound, module_unknown = _shim_bound_names(
            tree, loaders, top_level_only=True, seed=module_bound)
        for fname, fn in funcs.items():
            local, _ = _shim_bound_names(fn, loaders, seed=module_bound)
            if fname not in loaders and any(
                    isinstance(r, ast.Return) and isinstance(r.value, ast.Name)
                    and r.value.id in local for r in ast.walk(fn)):
                loaders.add(fname)
            # A `global X` rebound to the shim inside a helper is visible to the
            # whole module, which is exactly the caching idiom above.
            for g in (n for n in ast.walk(fn) if isinstance(n, ast.Global)):
                module_bound |= {nm for nm in g.names if nm in local}
        if (len(loaders), len(module_bound)) == before:
            break
    unknown += module_unknown

    # Seeded per scope, then propagated across LOCAL call boundaries to a
    # fixpoint. Handing the module to a helper in the same file is the ordinary
    # shape here (`todo-orphan-check.py` threads `_fence()` through its walkers,
    # the fence tests pass `tf` into assertion helpers), and treating it as
    # opaque made every run report UNKNOWN -- which suppresses DEAD, so the
    # check could never fail on the very defect it was built to catch. Following
    # the argument removes the uncertainty instead of tolerating it.
    seeded: dict[str, set[str]] = {}
    for scope in scopes:
        local, unk = _shim_bound_names(scope, loaders, seed=module_bound)
        seeded[id(scope)] = module_bound | local
        if scope is not tree:
            unknown += unk

    for _ in range(8):  # bounded: parameter depth in this tree is 1-2
        changed = False
        for scope in scopes:
            for node in ast.walk(scope):
                if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name):
                    continue
                callee = funcs.get(node.func.id)
                if callee is None:
                    continue
                # POSITIONAL-ONLY PARAMETERS FIRST: `def use(mod, /)` stores
                # `mod` in `args.posonlyargs`, not `args.args`, and this used to
                # count only the latter -- so a call through a positional-only
                # parameter matched no name at all and vanished with neither a
                # hit nor uncertainty.
                params = [a.arg for a in callee.args.posonlyargs] \
                       + [a.arg for a in callee.args.args]
                for i, a in enumerate(node.args):
                    if not (isinstance(a, ast.Name) and a.id in seeded[id(scope)]):
                        continue
                    if i < len(params):
                        if params[i] not in seeded[id(callee)]:
                            seeded[id(callee)].add(params[i])
                            changed = True
                    elif callee.args.vararg is not None:
                        # `*args` absorbs it, and this file cannot see through a
                        # tuple subscript to know which position it landed at.
                        unknown.append(f"shim module passed into {node.func.id!r} "
                                       f"via *{callee.args.vararg.arg}")
                    else:
                        unknown.append(f"shim module passed to {node.func.id!r} "
                                       f"past its declared parameters")
                # KEYWORDS TOO. Following positions only left `helper(mod=tf)`
                # silently unaccounted: the call is to a LOCAL function, so the
                # opaque-escape check skips it on the grounds that propagation
                # covers it, and propagation then did not.
                kwnames = params + [a.arg for a in getattr(callee.args, "kwonlyargs", [])]
                for k in node.keywords:
                    if not (isinstance(k.value, ast.Name)
                            and k.value.id in seeded[id(scope)]):
                        continue
                    if k.arg in kwnames:
                        if k.arg not in seeded[id(callee)]:
                            seeded[id(callee)].add(k.arg)
                            changed = True
                    else:
                        # Swallowed by `**kwargs`, or simply unmatched: either
                        # way this file cannot say where the module went.
                        unknown.append(f"shim module passed as keyword "
                                       f"{k.arg or '**'} into {node.func.id!r}")
        if not changed:
            break
    else:
        # THE CAP WAS REACHED WHILE STILL CHANGING. An 8-deep helper chain is
        # not hypothetical in a repo whose own tooling nests loaders this way,
        # and stopping silently there is a false DEAD for anything past the
        # cap -- so the run says it stopped rather than claiming completeness.
        unknown.append("binding propagation did not reach a fixpoint within "
                       "8 iterations (a helper chain deeper than 8)")

    for scope in scopes:
        bound = seeded[id(scope)]
        if not bound:
            continue
        for node in ast.walk(scope):
            # A shim-bound name STORED IN A CONTAINER escapes name resolution
            # entirely: `registry = {"fence": tf}` followed by
            # `registry["fence"].alpha([])` is an Attribute on a Subscript, not
            # on a Name, so it produced no hit AND no uncertainty -- every
            # export read DEAD against a consumer that plainly uses them.
            if isinstance(node, ast.Assign) and isinstance(node.value, ast.Name) \
                    and node.value.id in bound:
                for t in node.targets:
                    if not isinstance(t, (ast.Attribute, ast.Subscript)):
                        continue
                    # MEMOISING ONTO A LOCAL FUNCTION is not an escape.
                    # `_fence._mod = mod` (todo-staged-check.py:65) parks the
                    # module on the loader that produced it, and the only way
                    # back out is that loader's return -- which is already
                    # resolved. Flagging it raised a permanent UNKNOWN, and an
                    # UNKNOWN suppresses DEAD, so the ordinary memoise idiom
                    # silently disarmed the reachability half.
                    if isinstance(t, ast.Attribute) and isinstance(t.value, ast.Name) \
                            and t.value.id in loaders:
                        continue
                    unknown.append(f"shim module stored on an attribute or "
                                   f"subscript from {node.value.id!r}")
            if isinstance(node, (ast.Dict, ast.List, ast.Tuple, ast.Set)):
                elts = list(getattr(node, "values", [])) + list(getattr(node, "elts", []))
                for e in elts:
                    if isinstance(e, ast.Name) and e.id in bound:
                        unknown.append(f"shim module stored in a container "
                                       f"as {e.id!r}")
            if isinstance(node, ast.Attribute) and node.attr in exports \
                    and isinstance(node.value, ast.Name) and node.value.id in bound:
                hits.add(node.attr)
            elif isinstance(node, ast.Call):
                if isinstance(node.func, ast.Name) and node.func.id == "getattr" \
                        and node.args and isinstance(node.args[0], ast.Name) \
                        and node.args[0].id in bound:
                    unknown.append("getattr on the shim module")
                # Three callees are NOT opaque, and the reason is the same in
                # each: the reference, if any, is visible somewhere this check
                # already looks. `exec_module(mod)` is loader mechanics and RUNS
                # the module rather than calling an export. A LOCAL function was
                # followed by the propagation loop above. And an attribute call
                # -- `orph.scan_file(p, tri, fence)` in the fence tests -- hands
                # the module to a function of another TRACKED module, whose own
                # file is scanned independently, so an export used in there is
                # counted at its own definition site.
                # `exec_module(mod)` RUNS the module rather than calling an
                # export, and a LOCAL function was followed by the propagation
                # loop above. Everything else is an escape, INCLUDING an
                # attribute call: the earlier version skipped those on the
                # assumption that the receiving module would recognise an
                # injected parameter as the shim, and it cannot --
                # `helper.use(tf)` left `helper.py` with an ordinary parameter
                # and produced neither a hit nor uncertainty.
                # REGISTERING the module is not USING it. `exec_module(mod)`
                # runs it, and `sys.modules[...] = mod` publishes it under a
                # name; neither can call an export, and both appear in every
                # by-path loader in this tree -- so treating them as escapes
                # meant the loaders themselves suppressed every DEAD verdict.
                # `exec_module(mod)` RUNS the module rather than calling an
                # export, and a LOCAL function was followed by the propagation
                # loop above. `sys.modules` registration is NOT exempted: it
                # publishes the shim for a later dynamic import this scan cannot
                # follow, which is a real escape however ordinary it looks.
                if _call_name(node.func) == "exec_module" \
                        or (isinstance(node.func, ast.Name) and node.func.id in funcs):
                    continue
                for a in node.args:
                    if isinstance(a, ast.Name) and a.id in bound:
                        unknown.append(f"shim module passed to an opaque callable "
                                       f"as {a.id!r}")
                # A KEYWORD escape was never checked at all: `helper(mod=tf)`
                # produced neither a hit nor an UNKNOWN, so every export read
                # DEAD. Keywords escape exactly as positionally-passed ones do,
                # and the propagation loop above only follows positions.
                for k in node.keywords:
                    if isinstance(k.value, ast.Name) and k.value.id in bound:
                        unknown.append(f"shim module passed as keyword "
                                       f"{k.arg or '**'}={k.value.id!r}")

    # Shapes that are scope-independent: a direct `from todo_fence import X`,
    # and `loader().export` where the helper returns the module.
    for node in ast.walk(tree):
        if isinstance(node, ast.ImportFrom) and (
                node.module == "todo_fence"
                or (node.module or "").endswith(".todo_fence")):
            # `from scripts.todo_fence import alpha` is a direct reference to
            # the export and reads no differently from the unqualified spelling.
            hits |= {a.name for a in node.names if a.name != "*"}
        elif isinstance(node, ast.Attribute) and node.attr in exports \
                and isinstance(node.value, ast.Call) \
                and isinstance(node.value.func, ast.Name) \
                and node.value.func.id in loaders:
            # `todo-staged-check.py:794` uses `_fence().index_tree(...)`;
            # missing this shape reported a live export as DEAD.
            hits.add(node.attr)

    return hits, unknown


def scan_shell(path: Path, exports: set[str]) -> tuple[set[str], list[str]]:
    """`(hits, unknown)` -- LIVE-only evidence from an embedded heredoc.

    A shell file that NAMES the shim but yields no resolvable reference is
    UNCERTAIN, not empty. The heredoc is real Python that this function only
    pattern-matches, so `from todo_fence import alpha` followed by a bare
    `alpha([])` is a live call it cannot see; reporting nothing there is a false
    DEAD, and a false DEAD is the one verdict that gets live code deleted.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        # An undecodable file is not a file with no callers in it.
        return set(), ["not decodable as UTF-8"]
    body = "\n".join(l for l in text.split("\n") if not l.lstrip().startswith("#"))
    # QUALIFY ON THE BODY, not the raw text. `todo-graph/tests/test_build.sh`
    # names `todo_fence` only in comments while importing `fence_mask` from
    # cache_schema directly, so a raw-text gate credited the shim with a caller
    # that belongs to a different module -- and `fence_mask` read TEST-ONLY when
    # it has no caller at all.
    # AN IMPORT-SHAPED REFERENCE, not any mention of the name. A shell file can
    # legitimately name the shim without consuming it -- `test-tooling.sh:174`
    # copies `scripts/todo_fence.py` into a sandbox, and its test registration
    # names `test_todo_fence.py` -- and treating those as consumers raised a
    # permanent UNKNOWN, which suppresses DEAD and left the reachability half
    # inert. Only a heredoc that actually LOADS the module can hide a call.
    if not re.search(r"import\s+[\w.]*todo_fence\b"
                     r"|from\s+[\w.]*todo_fence\s+import"
                     r"|spec_from_file_location\s*\(\s*['\"]todo_fence", body):
        return set(), []
    # ATTRIBUTE-ON-AN-IDENTIFIER, not a bare token. `lint.sh:1055` contains
    # `_fence.scan_text(raw).unclosed_reason()`, where the first is a shim call
    # and the second is a method on the returned `ScanResult` -- the same
    # producer/consumer name collision that makes a textual scan wrong on the
    # Python side. Requiring `ident.NAME` credits `_fence.mask_text(...)` and
    # `_fence.StagedSnapshotError` while leaving `).unclosed_reason()` alone.
    hits = {n for n in exports
            if re.search(r"[A-Za-z_][A-Za-z0-9_]*\." + re.escape(n) + r"\b", body)}
    # `from todo_fence import alpha, beta` names the exports directly, with no
    # module prefix for the attribute pattern to find.
    for m in re.finditer(r"from\s+[\w.]*todo_fence\s+import\s+([^\n;]+)", body):
        for part in m.group(1).replace("(", " ").replace(")", " ").split(","):
            nm = part.strip().split(" as ")[0].strip()
            if nm == "*":
                return hits, ["star-import of todo_fence in a heredoc"]
            if nm in exports:
                hits.add(nm)
    if not hits:
        return hits, ["names todo_fence but no reference resolves -- an embedded "
                      "heredoc this scan can only pattern-match"]
    return hits, []


def signature_drift(shim, forwards: dict[str, str]) -> list[str]:
    """Wrapper-vs-target parameter drift, ANNOTATIONS DELIBERATELY IGNORED.

    Raw `inspect.signature` equality is unusable here, and that is measured
    rather than assumed: the shim carries `from __future__ import annotations`,
    so its annotations are strings while the target's are objects, and several
    targets annotate a return the shim does not. On the unchanged tree 8 of 10
    wrappers "differ" that way before any real drift exists -- a check built on
    equality would fail a healthy tree, and the only escapes are to weaken it or
    to copy the target's typing into a shim whose whole purpose is not to know
    the target's types.

    What genuinely matters is the PARAMETER CONTRACT: a target that grows an
    optional parameter is silently unreachable through a fixed-arity wrapper,
    and no test fails, because nothing calls it.
    """
    cs = shim._cache_schema()
    out: list[str] = []
    for name, target_attr in sorted(forwards.items()):
        w = getattr(shim, name, None)
        t = getattr(cs, target_attr, None)
        if w is None:
            out.append(f"{name}: exported but not defined in the shim")
            continue
        if t is None:
            out.append(f"{name}: forwards to cache_schema.{target_attr}, which "
                       f"does not exist")
            continue
        wp = list(inspect.signature(w).parameters.values())
        tp = list(inspect.signature(t).parameters.values())
        if any(p.kind in (p.VAR_POSITIONAL, p.VAR_KEYWORD) for p in wp):
            # A transparent forwarder has nothing fixed to compare; reporting
            # drift against it would be reporting drift that cannot exist.
            continue
        def _default(p):
            if p.default is p.empty:
                return (False, None)
            try:
                # `repr` rather than the value: an unhashable or
                # oddly-comparing default must not make the check itself throw,
                # and a changed repr is a changed default either way.
                return (True, repr(p.default))
            except Exception:
                return (True, "<unrepresentable>")

        wk = [(p.name, p.kind, _default(p)) for p in wp]
        tk = [(p.name, p.kind, _default(p)) for p in tp]
        if wk != tk:
            out.append(f"{name}: forwards to cache_schema.{target_attr} but the "
                       f"parameter contract differs -- shim "
                       f"({', '.join(p.name for p in wp) or 'no parameters'}) vs "
                       f"target ({', '.join(p.name for p in tp) or 'no parameters'})")
    return out


def load_shim(path: Path):
    import importlib.util
    # NOT registered in `sys.modules`: nothing here imports the shim by name,
    # and publishing it would hand it to any later dynamic import -- an escape
    # this scan cannot follow, in the one file that has no excuse for one.
    spec = importlib.util.spec_from_file_location("todo_fence", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def analyse(root: Path):
    """`(names, live, testonly, unknown, drift)` for the whole tracked tree."""
    shim_path = root / SHIM_REL
    names, forwards = shim_exports(shim_path)
    exports = set(names)

    live: dict[str, set[str]] = {n: set() for n in names}
    testonly: dict[str, set[str]] = {n: set() for n in names}
    unknown: list[str] = []

    for rel in _tracked(root):
        if rel == SHIM_REL:
            continue
        p = root / rel
        if not p.is_file():
            continue
        if rel.endswith(".py"):
            hits, unk = scan_python(p, rel, exports)
            unknown += [f"{rel}: {u}" for u in unk]
        else:
            hits, unk = scan_shell(p, exports)
            unknown += [f"{rel}: {u}" for u in unk]
        for h in hits:
            (testonly if _is_test(rel) else live)[h].add(rel)

    drift = signature_drift(load_shim(shim_path), forwards)
    return names, live, testonly, unknown, drift


def main(argv: list[str]) -> int:
    root = Path(argv[1] if len(argv) > 1 else ".").resolve()
    if not (root / SHIM_REL).is_file():
        # Not FATAL: lint.sh already gates on this file existing before it ever
        # invokes the checker, so this path is reachable only from a standalone
        # or test invocation against a root that genuinely lacks the shim --
        # advisory, same as every other "nothing to say" outcome.
        print(f"[check_alias_staleness] {SHIM_REL} not found under {root}")
        return 1
    try:
        names, live, testonly, unknown, drift = analyse(root)
    except NotAGitRepo:
        # Nothing to report: the tracked-tree closure this checker reasons
        # about does not exist here. Not a finding, not a malfunction.
        return 0

    dead = [n for n in names if not live[n] and not testonly[n]]
    only_tests = [n for n in names if not live[n] and testonly[n]]

    # SUPPRESSED, not merely discounted. Printing "DEAD: x" beside "reachability
    # is unknown" invites exactly the unsafe retirement the suppression rule
    # exists to prevent, and the earlier version printed both because it
    # computed the verdicts before it looked at `unknown`.
    if not unknown:
        for n in sorted(only_tests):
            print(f"[check_alias_staleness] TEST-ONLY: {n} -- referenced only by "
                  f"{', '.join(sorted(testonly[n]))}; no production caller "
                  f"reaches this export")
        for n in sorted(dead):
            print(f"[check_alias_staleness] DEAD: {n} -- no reference resolves "
                  f"to this export anywhere in the tracked tree")
    for d in drift:
        print(f"[check_alias_staleness] DRIFT: {d}")

    if unknown:
        # An UNKNOWN records that the checker stepped back from a DEAD verdict
        # it could not justify, which is the safe way to be wrong -- but it is
        # NOT a quiet outcome. It suppresses DEAD and TEST-ONLY, so if it ever
        # became routine the check would go on passing while proving nothing;
        # that is exactly what happened when this file's own fixtures were
        # tracked. It never suppresses DRIFT, which is a property of two
        # signatures and has nothing to do with reachability.
        for u in sorted(set(unknown)):
            print(f"[check_alias_staleness] UNKNOWN (suppresses DEAD): {u}")

    # THE EXIT CODE SEPARATES A VERDICT FROM A MALFUNCTION, and nothing else.
    #   1 = advisory: every verdict this tool produces (DRIFT, DEAD, TEST-ONLY,
    #       UNKNOWN). A human reads it and decides.
    #   3 = FATAL: the check could not run. Reserved and distinct because the
    #       obvious spelling -- letting an exception exit 1 like Python does --
    #       is indistinguishable from an advisory result, so a crash reads as a
    #       clean advisory pass and the check silently stops existing.
    #   0 = nothing to say.
    #
    # NO BLOCKING VERDICT, decided after three review rounds argued it both
    # ways. Erroring on DRIFT was tried and withdrawn on evidence: a target
    # parameter RENAME with positional forwarding still working, and a
    # transparent decorator exposing `(*args, **kwargs)`, both produce DRIFT
    # while behaving identically, so the enforcing arm blocks healthy
    # refactors. A check whose false positives stop work gets skipped, and a
    # skipped check is worth less than a noisy one. The class this promotes was
    # never suffering from a missing refusal -- it was suffering from nobody
    # LOOKING, and a warning that names the export at every lint run is the
    # thing that was actually absent.
    return 1 if (drift or dead or only_tests or unknown) else 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except SystemExit:
        raise
    except BaseException as exc:                       # noqa: BLE001
        # Every failure route lands here, including `CheckerFailed`.
        # FAIL LOUD AND DISTINCT. An uncaught exception exits 1, which is this
        # tool's ADVISORY code, so a checker that crashed would be read as a
        # checker that ran and found something mild -- and lint, seeing no
        # prefixed verdict line, would add nothing at all.
        print(f"[check_alias_staleness] FATAL: {type(exc).__name__}: {exc}")
        sys.exit(3)
