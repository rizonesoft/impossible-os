#!/usr/bin/env python3
"""Lint: the site tooling fails closed and never waits without a bound.

Two review classes promoted into automation, the way check_parsers.py promoted
`parser-approximation`. Measured 2026-09-30 with
`python3 scripts/overnight/finding-ledger.py classes`: `fail-open` 12 fixed and
`unbounded-wait` 4 fixed, nearly all in the retained-release work, where missing
or unreadable state kept being read as "empty" (a store without a manifest, a
404 picker, an absent branch) and waits had no overall bound (a trickling HTTP
body, a stalled git fetch). Each was caught by a Codex round.

Rule `fail-direction`. An empty value produced where input is missing or an
error was caught is a claim that "nothing" is the safe reading. The rule
reports, in `scripts/site/*.py`:

  - a `return` of an empty value (a bare `return`, `None`, `""`, `b""`, `[]`,
    `{}`, `()`, or `set()`/`dict()`/`list()`/`tuple()`/`frozenset()` with no
    arguments), or an assignment of one to a name or attribute, when it sits
    in an `except` handler, or in either branch of an `if` whose test probes
    for missing input: `.exists()`, `.is_file()`, `.is_dir()`, `.is_symlink()`,
    `os.path.exists/lexists/isfile/isdir`, `os.access`, a `.returncode`, a
    `subprocess.run(...)` call, or a function of the same file that returns one;
  - a conditional expression whose test probes for missing input and one of
    whose arms is empty (`x if p.is_dir() else set()`), anywhere in a returned or
    assigned value, lambda bodies included; each is its own finding at its own line;
  - `contextlib.suppress(...)`, which is an except handler with no body.

`(x := None)` counts as the value it binds. A branch that first records the
failure on the tools' error channel (an `.append` on a list named `errors`,
which every site entry point turns into a non-zero exit; not `.extend`, which
may add nothing) is failing closed and is not reported. `pass` and
`continue` in a handler are outside the rule: skipping one item after
recording why is the common correct shape, and a blanket ban would bury it.

Waiver, on one of the statement's lines or standing alone on the line above:

    return {}   # fail-direction: <why empty is the safe reading, 20+ chars>

Rule `bounded-wait`. A wait with no bound can hold a CI job until the
workflow's own limit kills it, which reports nothing useful. The rule reports:

  - `subprocess.run/call/check_call/check_output` without a finite `timeout=`;
  - `subprocess.Popen(...)`, always: it has no timeout parameter, so the
    comment names where the bound on its pipes and exit actually lives;
  - `.wait()`, `.wait_for()`, `.join()` and `.communicate()` without a finite
    timeout, read by signature (`wait(t)`, `wait_for(pred, t)`,
    `communicate(input, t)`; `join` with no arguments or a visible `None`, since
    `str.join` takes an iterable);
  - `urllib.request.urlopen(...)` and `.open(...)` on an opener built by
    `build_opener`, held in a name or an attribute (`self.opener`), directly or
    through a function of the same file, always:
    a socket timeout bounds each read, not the body, so a body that trickles
    in forever passes it. Without a finite timeout it is reported whatever
    the comment says.

A timeout is not finite when it is missing, the constant `None`, a conditional
expression with a `None` arm, a `**` splat, or a name the scope (or an
enclosing one, unless the scope rebinds it) binds to one of those by an
assignment, a walrus or a parameter default. Names bound only as a `for`,
comprehension or `with` target, or by unpacking, are not traced (the check is
flow-insensitive syntax, not data flow). The waiver names the caller-owned bound:

    proc = subprocess.Popen(...)   # deadline: <owner of the bound, 20+ chars>

A finding owns the waiver on its first line, else one standing alone on the
line above, else one on a later line of its own span that no other finding
starts on, so an inner call's waiver never excuses the outer call. A waiver
claimed by two findings excuses neither, and one with nothing to waive is
reported as stale. Names reached only
through `getattr`, `functools.partial` or a helper in another module are out of
scope: the rules read one file's syntax, which is what a review round reads too.

`--control` runs each rule over its fixtures under
scripts/site/tests/fixtures/<rule>/, whose lines are marked `# expect: flag` or
`# expect: clean`, and fails unless every marked line is judged as marked.

Usage: python3 scripts/site/check_nets.py --rule fail-direction|bounded-wait [--control] [FILE ...]
Exit: 0 clean, 1 findings (or a control that did not fire), 2 usage error.
"""

from __future__ import annotations

import ast
import io
import sys
import tokenize
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
FIXTURES = HERE / "tests" / "fixtures"
MIN_REASON = 20
RULES = {"fail-direction": "fail-direction:", "bounded-wait": "deadline:"}

EMPTY_CALLS = frozenset({"set", "dict", "list", "tuple", "frozenset"})
PROBE_METHODS = frozenset({"exists", "is_file", "is_dir", "is_symlink"})
PROBE_FUNCS = frozenset({"os.path.exists", "os.path.lexists", "os.path.isfile", "os.path.isdir", "os.access",
                         "subprocess.run"})
SUBPROCESS_TIMED = frozenset({"subprocess.run", "subprocess.call", "subprocess.check_call",
                              "subprocess.check_output"})
ERROR_CHANNEL = "errors"
DEFS = (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef, ast.Lambda)
COMPS = (ast.ListComp, ast.SetComp, ast.DictComp, ast.GeneratorExp)


def _declared_global(body: list[ast.stmt]) -> set[str]:
    """Names ONE scope declares `global` (not its nested functions' declarations)."""
    out: set[str] = set()
    stack = list(body)
    while stack:
        node = stack.pop()
        if isinstance(node, DEFS):
            continue
        if isinstance(node, ast.Global):
            out.update(node.names)
        stack.extend(ast.iter_child_nodes(node))
    return out


def _outside_body(node: ast.AST) -> list[ast.AST]:
    """The parts of a function, lambda or class definition that run where it is
    DEFINED: everything but its body (defaults, annotations, decorators, bases,
    keywords, type parameters)."""
    body = node.body if isinstance(node.body, list) else [node.body]
    inside = {id(b) for b in body}
    return [c for c in ast.iter_child_nodes(node) if id(c) not in inside]


def _comprehension_parts(node: ast.expr) -> tuple[ast.expr, list[ast.AST]]:
    """(the first iterable, which runs where the comprehension is written; every other
    part, which runs in the comprehension's own scope and so looks past a class body)."""
    first = node.generators[0].iter
    rest: list[ast.AST] = []
    for child in ast.iter_child_nodes(node):
        if isinstance(child, ast.comprehension):
            rest += [part for part in ast.iter_child_nodes(child) if part is not first]
        else:
            rest.append(child)
    return first, rest


def _imports(body: list[ast.stmt], parent: dict[str, str] | None = None) -> dict[str, str]:
    """{local name: dotted origin} of the imports ONE scope makes (not those of the
    functions and classes nested in it), over its enclosing scope's: an import local
    to one function cannot re-bind a name another function resolves."""
    out: dict[str, str] = dict(parent or {})
    stack = list(body)
    while stack:
        node = stack.pop(0)
        if isinstance(node, DEFS):
            continue
        stack.extend(ast.iter_child_nodes(node))
        if isinstance(node, ast.Import):
            for a in node.names:
                if a.asname:
                    out[a.asname] = a.name
                else:
                    top = a.name.split(".")[0]
                    out[top] = top
        elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
            for a in node.names:
                out[a.asname or a.name] = f"{node.module}.{a.name}"
    return out


def _scope_envs(tree: ast.Module) -> dict[int, dict[str, str]]:
    """{id(node): the import environment the node runs in}, for every node: the one
    place scoping is decided, by Python's rules: a function body runs in its own
    imports over its enclosing FUNCTION's, and a class body in its own over the same,
    so no class body's names reach a method, a nested class or a comprehension; a
    comprehension's first iterable runs where it is written and the rest in the
    comprehension's own scope; `global` names resolve at module level; decorators,
    defaults and bases run where the definition is. `nonlocal` writes are not
    followed back to the owning function (a declared limit)."""
    envs: dict[int, dict[str, str]] = {}

    def visit(node: ast.AST, env: dict[str, str], closure: dict[str, str]) -> None:
        envs[id(node)] = env
        if isinstance(node, COMPS):
            first, rest = _comprehension_parts(node)
            visit(first, env, closure)
            for part in rest:
                visit(part, closure, closure)
            return
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda, ast.ClassDef)) and node is not tree:
            body = node.body if isinstance(node.body, list) else [node.body]
            inside = {id(b) for b in body}
            if isinstance(node, ast.Lambda):
                inner = inner_closure = closure
            else:
                inner = _imports(body, closure)
                for name in _declared_global(body):   # a class body may declare `global` too
                    if name in top:
                        inner[name] = top[name]
                    else:
                        inner.pop(name, None)
                inner_closure = closure if isinstance(node, ast.ClassDef) else inner
            for part in ast.iter_child_nodes(node):
                if id(part) in inside:
                    visit(part, inner, inner_closure)
                else:
                    visit(part, env, closure)
            return
        for child in ast.iter_child_nodes(node):
            visit(child, env, closure)

    top = _imports(tree.body)
    visit(tree, top, top)
    return envs


def _qual(func: ast.expr, imports: dict[str, str]) -> str:
    """The dotted origin of a call target (`sp.run` -> `subprocess.run`), "" when unknown."""
    parts: list[str] = []
    while isinstance(func, ast.Attribute):
        parts.append(func.attr)
        func = func.value
    if not isinstance(func, ast.Name) or func.id not in imports:
        return ""
    return ".".join([imports[func.id], *reversed(parts)])


def _is_empty(v: ast.expr | None) -> bool:
    while isinstance(v, ast.NamedExpr):   # `(x := None)` is the value it binds
        v = v.value
    if v is None:
        return True
    if isinstance(v, ast.Constant):
        return v.value is None or v.value in ("", b"")
    if isinstance(v, (ast.List, ast.Tuple, ast.Set)):
        return not v.elts
    if isinstance(v, ast.Dict):
        return not v.keys
    return (isinstance(v, ast.Call) and isinstance(v.func, ast.Name) and v.func.id in EMPTY_CALLS
            and not v.args and not v.keywords)


def _maybe_empty(v: ast.expr | None) -> bool:
    """Empty, or a conditional expression one of whose arms may be."""
    while isinstance(v, ast.NamedExpr):
        v = v.value
    if isinstance(v, ast.IfExp):
        return _maybe_empty(v.body) or _maybe_empty(v.orelse)
    return _is_empty(v)


def _walk_expr(node: ast.AST):
    """Every node of an expression, lambda bodies included (a fallback inside one is still a fallback)."""
    stack = [node]
    while stack:
        n = stack.pop()
        yield n
        stack.extend(ast.iter_child_nodes(n))


def _probe(test: ast.expr, envs: dict[int, dict[str, str]], helpers: frozenset[str] = frozenset()) -> str:
    """The missing-input probe inside an `if` test ("" when there is none), each call
    resolved in the environment IT runs in (a lambda inside the test included).
    `helpers` are this file's functions that return a probe, so wrapping one hides nothing."""
    for n in _walk_expr(test):
        imports = envs.get(id(n), {})
        if isinstance(n, ast.Attribute) and n.attr == "returncode":
            return ast.unparse(n)[:60]
        if isinstance(n, ast.Call):
            f = n.func
            if isinstance(f, ast.Attribute) and f.attr in PROBE_METHODS and not _qual(f, imports):
                return ast.unparse(f)[:60] + "()"
            if _qual(f, imports) in PROBE_FUNCS:
                return _qual(f, imports) + "()"
            if isinstance(f, ast.Name) and f.id in helpers:
                return f.id + "()"
    return ""


def _probe_helpers(tree: ast.Module, envs: dict[int, dict[str, str]]) -> frozenset[str]:
    """Functions of this file that return a missing-input probe, directly or through
    another such function (to a fixpoint), each judged in its own scope's imports."""
    funcs = [f for f in ast.walk(tree) if isinstance(f, (ast.FunctionDef, ast.AsyncFunctionDef))]
    found: frozenset[str] = frozenset()
    while True:
        more = frozenset(f.name for f in funcs if any(
            isinstance(r, ast.Return) and r.value is not None and _probe(r.value, envs, found)
            for r in ast.walk(f)))
        if more <= found:
            return found
        found |= more


def _is_suppress(expr: ast.expr, imports: dict[str, str]) -> bool:
    return isinstance(expr, ast.Call) and _qual(expr.func, imports) == "contextlib.suppress"


def _records_error(stmt: ast.stmt) -> bool:
    """`errors.append(...)` / `self.errors.append(...)`: the failure is on the error channel.
    Only `append` counts: it always adds an entry, where `extend` may add none."""
    if not (isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Call)):
        return False
    f = stmt.value.func
    if not (isinstance(f, ast.Attribute) and f.attr == "append"):
        return False
    recv = f.value
    name = recv.attr if isinstance(recv, ast.Attribute) else recv.id if isinstance(recv, ast.Name) else ""
    return name == ERROR_CHANNEL


class FailDirection:
    """Report empty values produced on a failure or missing-input path."""

    def __init__(self, tree: ast.Module) -> None:
        self.envs = _scope_envs(tree)
        self.helpers = _probe_helpers(tree, self.envs)
        self.found: list[tuple[int, int, int, str]] = []   # (first line, last line, column, message)
        self.block(tree.body, "", False)

    def report(self, node: ast.AST, why: str) -> None:
        self.found.append((node.lineno, node.end_lineno or node.lineno, node.col_offset, why))

    def block(self, stmts: list[ast.stmt], ctx: str, recorded: bool) -> None:
        for s in stmts:
            if _records_error(s):
                recorded = True
            self.stmt(s, ctx, recorded)

    def value(self, stmt: ast.stmt, v: ast.expr | None, what: str, ctx: str, recorded: bool) -> None:
        if recorded:
            return
        if ctx and _maybe_empty(v):
            self.report(stmt, f"{what} an empty value in {ctx}")
            return
        # Each conditional is its own finding at its own line, so a waiver on one
        # cannot excuse another in the same statement.
        for n in _walk_expr(v) if v is not None else ():
            if isinstance(n, ast.IfExp) and (_maybe_empty(n.body) or _maybe_empty(n.orelse)):
                probe = _probe(n.test, self.envs, self.helpers)
                if probe:
                    self.report(n, f"{what} an empty value when {probe} says the input is missing")

    def stmt(self, s: ast.stmt, ctx: str, recorded: bool) -> None:
        if isinstance(s, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            self.block(s.body, "", False)          # a nested scope starts clean
        elif isinstance(s, (ast.Try, getattr(ast, "TryStar", ast.Try))):
            self.block(s.body, ctx, recorded)
            for h in s.handlers:
                self.block(h.body, "an except handler", recorded)
            self.block(s.orelse, ctx, recorded)
            self.block(s.finalbody, ctx, recorded)
        elif isinstance(s, ast.If):
            probe = _probe(s.test, self.envs, self.helpers)
            inner = f"a missing-input branch ({probe})" if probe else ctx
            self.block(s.body, inner, recorded)
            self.block(s.orelse, inner, recorded)
        elif isinstance(s, (ast.With, ast.AsyncWith)):
            for item in s.items:
                if _is_suppress(item.context_expr, self.envs[id(item.context_expr)]):
                    self.found.append((s.lineno, item.context_expr.end_lineno or s.lineno, item.context_expr.col_offset,
                                       "contextlib.suppress swallows the error with no fail direction"))
            self.block(s.body, ctx, recorded)
        elif isinstance(s, (ast.For, ast.AsyncFor, ast.While)):
            self.block(s.body, ctx, recorded)
            self.block(s.orelse, ctx, recorded)
        elif isinstance(s, ast.Match):
            for case in s.cases:
                self.block(case.body, ctx, recorded)
        elif isinstance(s, ast.Return):
            self.value(s, s.value, "returns", ctx, recorded)
        elif isinstance(s, (ast.Assign, ast.AnnAssign)) and s.value is not None:
            targets = s.targets if isinstance(s, ast.Assign) else [s.target]
            if all(isinstance(t, (ast.Name, ast.Attribute)) for t in targets):
                self.value(s, s.value, "assigns", ctx, recorded)


class BoundedWait:
    """Report waits whose bound is missing or not visible at the call."""

    def __init__(self, tree: ast.Module) -> None:
        self.envs = _scope_envs(tree)
        self.found: list[tuple[int, int, int, str]] = []   # (first line, last line, column, message)
        self.always: list[tuple[int, int, int, str]] = []  # reported even under a waiver
        self.module_bindings = self._scoped({}, self._bindings(tree.body, []))
        self.openers = self._openers(tree)
        self.scope(tree.body, self.module_bindings, self.module_bindings)

    @staticmethod
    def _bindings(body: list[ast.stmt], args: list[tuple[ast.arg, ast.expr | None]]) -> dict[str, list[ast.expr]]:
        """{name: every value bound to it in this scope, parameter defaults included}."""
        out: dict[str, list[ast.expr]] = {}
        for a, default in args:
            out.setdefault(a.arg, []).append(default if default is not None else ast.Constant(value=0))
        stack = list(body)
        while stack:
            n = stack.pop()
            if isinstance(n, DEFS):
                # Its body is another scope, but the rest of the definition runs HERE,
                # so a walrus in a default or an annotation binds in this scope.
                stack.extend(_outside_body(n))
                continue
            if isinstance(n, (ast.Assign, ast.AnnAssign)) and n.value is not None:
                for t in n.targets if isinstance(n, ast.Assign) else [n.target]:
                    if isinstance(t, ast.Name):
                        out.setdefault(t.id, []).append(n.value)
            elif isinstance(n, ast.NamedExpr):
                out.setdefault(n.target.id, []).append(n.value)
            stack.extend(ast.iter_child_nodes(n))
        return out

    def _openers(self, tree: ast.Module) -> set[str]:
        """Receivers bound to a urllib opener (`OPENER`, `self.opener`): `build_opener(...)`
        directly, or a call of a function in this file that returns one."""
        def builds(v: ast.expr | None) -> bool:
            return isinstance(v, ast.Call) and _qual(v.func, self.envs[id(v)]) in (
                "urllib.request.build_opener", "urllib.request.OpenerDirector")
        self.factories = {f.name for f in ast.walk(tree) if isinstance(f, (ast.FunctionDef, ast.AsyncFunctionDef))
                          and any(isinstance(r, ast.Return) and builds(r.value) for r in ast.walk(f))}
        self._builds = builds
        names: set[str] = set()
        for n in ast.walk(tree):
            if isinstance(n, (ast.Assign, ast.AnnAssign)) and n.value is not None:
                v = n.value
                if self.builds_opener(v):
                    for t in n.targets if isinstance(n, ast.Assign) else [n.target]:
                        if isinstance(t, (ast.Name, ast.Attribute)):
                            names.add(ast.unparse(t))
        return names

    @staticmethod
    def _scoped(parent: dict, raw: dict[str, list[ast.expr]]) -> dict:
        """A scope: PARENT's names, then this scope's own, each value paired with the
        scope it was written in, so an alias resolves where it was defined
        (`ALIAS = T` at module level reads the module's `T`, not a caller's)."""
        scope = dict(parent)
        for name, values in raw.items():
            scope[name] = [(v, scope) for v in values]
        return scope

    def _globals(self, scope: dict, body: list[ast.stmt], raw: dict[str, list[ast.expr]]) -> dict:
        """SCOPE with every name BODY declares `global` read from module level (plus
        any value this scope writes to it)."""
        for name in _declared_global(body):
            scope[name] = self.module_bindings.get(name, []) + [(v, scope) for v in raw.get(name, [])]
        return scope

    def builds_opener(self, v: ast.expr | None) -> bool:
        """`build_opener(...)` / `OpenerDirector()`, or a call of this file's factory for one."""
        return self._builds(v) or (isinstance(v, ast.Call) and isinstance(v.func, ast.Name)
                                   and v.func.id in self.factories)

    def scope(self, body: list[ast.stmt], bindings: dict[str, list[ast.expr]],
              closure: dict[str, list[ast.expr]]) -> None:
        """Walk one scope. `bindings` are the names its statements see; `closure` the
        names a function defined here closes over (a class body's are not among them)."""
        stack = list(body)
        while stack:
            n = stack.pop()
            if isinstance(n, ast.ClassDef):
                stack.extend(_outside_body(n))
                # A class body sees its own names over the enclosing FUNCTION's, never an
                # enclosing class's, and hands none of them to what it defines.
                raw = self._bindings(n.body, [])
                self.scope(n.body, self._globals(self._scoped(closure, raw), n.body, raw), closure)
                continue
            if isinstance(n, COMPS) and bindings is not closure:
                first, rest = _comprehension_parts(n)   # in a class body only the first iterable sees it
                stack.append(first)
                self.scope(rest, closure, closure)
                continue
            if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
                a = n.args
                pos = a.posonlyargs + a.args
                params = list(zip(pos, [None] * (len(pos) - len(a.defaults)) + a.defaults))
                params += list(zip(a.kwonlyargs, a.kw_defaults))
                for extra in (a.vararg, a.kwarg):
                    if extra is not None:
                        params.append((extra, None))
                inner = self._bindings(n.body if isinstance(n.body, list) else [ast.Expr(n.body)], [])
                # A nested scope sees its enclosing function's names unless it rebinds them.
                body = n.body if isinstance(n.body, list) else [n.body]
                scope = dict(closure)
                for arg, default in params:
                    # A default is evaluated where the function is DEFINED (a class body
                    # included); a parameter without one is supplied by the caller.
                    scope[arg.arg] = [(default, bindings)] if default is not None else [(ast.Constant(value=0), scope)]
                names = {arg.arg for arg, _ in params}
                for name, values in inner.items():   # a body assignment adds to a parameter, shadows the rest
                    scope[name] = (scope[name] if name in names else []) + [(v, scope) for v in values]
                scope = self._globals(scope, body, inner)
                self.scope(body, scope, scope)
                stack.extend(_outside_body(n))   # defaults, annotations and decorators run here
                continue
            if isinstance(n, ast.Call):
                self.call(n, bindings)
            stack.extend(ast.iter_child_nodes(n))

    def unbounded(self, v: ast.expr | None, bindings: dict[str, list[ast.expr]], seen: frozenset = frozenset()) -> bool:
        while isinstance(v, ast.NamedExpr):   # `timeout=(t := None)` is the value it binds
            v = v.value
        if v is None or (isinstance(v, ast.Constant) and v.value is None):
            return True
        if isinstance(v, ast.IfExp):
            return self.unbounded(v.body, bindings, seen) or self.unbounded(v.orelse, bindings, seen)
        key = (id(bindings), getattr(v, "id", None))
        if isinstance(v, ast.Name) and v.id in bindings and key not in seen:
            # Each value is resolved in the scope that wrote it, never the caller's.
            return any(self.unbounded(b, env, seen | {key}) for b, env in bindings[v.id])
        return False

    @staticmethod
    def arg(call: ast.Call, index: int, name: str) -> tuple[bool, ast.expr | None]:
        """(opaque, the argument) for the parameter at positional `index` or keyword `name`;
        opaque when a `*`/`**` splat could be carrying it."""
        for k in call.keywords:
            if k.arg == name:
                return False, k.value
        if any(k.arg is None for k in call.keywords):
            return True, None
        if index is None:
            return False, None
        if any(isinstance(a, ast.Starred) for a in call.args[:index + 1]):
            return True, None
        return False, call.args[index] if len(call.args) > index else None

    def call(self, call: ast.Call, bindings: dict[str, list[ast.expr]]) -> None:
        q = _qual(call.func, self.envs[id(call)])
        f = call.func
        method = f.attr if isinstance(f, ast.Attribute) and not q else ""
        span = (call.lineno, call.end_lineno or call.lineno, call.col_offset)

        def check(index, name: str, what: str, always: bool = False) -> None:
            opaque, v = self.arg(call, index, name)
            if opaque or self.unbounded(v, bindings):
                why = "a splat hides" if opaque else "no finite"
                msg = f"{what} with {why} timeout"
                (self.always if always else self.found).append(span + (msg,))

        if q in SUBPROCESS_TIMED:
            check(None, "timeout", q)
        elif q == "subprocess.Popen":
            self.found.append(span + ("subprocess.Popen has no timeout; name where the bound on its pipes and exit lives",))
        elif q == "urllib.request.urlopen" or (method == "open" and (ast.unparse(f.value) in self.openers
                                                                   or self.builds_opener(f.value))):
            what = q or f"{ast.unparse(f.value)}.open"
            check(2, "timeout", what, always=True)
            self.found.append(span + (f"{what}: a socket timeout bounds each read, not the body; name the overall deadline",))
        elif method in ("wait", "communicate", "wait_for"):
            check({"wait": 0, "communicate": 1, "wait_for": 1}[method], "timeout", f".{method}()")
        elif method == "join" and not call.args:
            check(None, "timeout", ".join()")
        elif method == "join" and len(call.args) == 1 and not call.keywords and self.unbounded(call.args[0], bindings):
            # `str.join` takes an iterable, so only a VISIBLE None (`join(None)`, a None arm) is a thread join.
            self.found.append(span + (".join() with no finite timeout",))


def _waivers(text: str, marker: str) -> dict[int, tuple[str, bool]]:
    """{line: (reason, stands alone on its line)} of every `# <marker>` COMMENT, read
    by the tokenizer so words inside strings and docstrings never count."""
    out: dict[int, tuple[str, bool]] = {}
    for tok in tokenize.generate_tokens(io.StringIO(text).readline):
        if tok.type == tokenize.COMMENT:
            body = tok.string[1:].strip()
            if body.startswith(marker):   # the reason ends where a second comment starts
                out[tok.start[0]] = (body[len(marker):].split(" #", 1)[0].strip(), tok.line.lstrip().startswith("#"))
    return out


def _comments(text: str) -> dict[int, str]:
    return {t.start[0]: t.string for t in tokenize.generate_tokens(io.StringIO(text).readline)
            if t.type == tokenize.COMMENT}


def check_file(path: Path, rule: str) -> list[tuple[int, str]]:
    """[(line, message)] for one file under one rule."""
    text = path.read_text(encoding="utf-8")
    try:
        tree = ast.parse(text, filename=str(path))
    except SyntaxError as e:
        return [(e.lineno or 1, f"cannot parse: {e.msg}")]
    marker = RULES[rule]
    scan = FailDirection(tree) if rule == "fail-direction" else BoundedWait(tree)
    always = getattr(scan, "always", [])
    waivers = _waivers(text, marker)
    used: set[int] = set()
    found: list[tuple[int, str]] = [(first, msg) for first, _, _, msg in always]
    # Which waiver each finding owns: one on its first line, else one standing alone
    # on the line above, else one on a later line of its own span that no other
    # finding starts on (so an inner call's waiver never excuses the outer call).
    starts = {first for first, _, _, _ in scan.found}
    claims: dict[int, list[tuple[int, str]]] = {}
    # Every finding keeps its identity (its column): two identical calls on one
    # line are two findings, so one waiver cannot quietly excuse both.
    for first, last, _col, msg in sorted(scan.found):
        if first in waivers:
            at = first
        elif first - 1 in waivers and waivers[first - 1][1]:
            at = first - 1
        else:
            at = next((n for n in range(first + 1, last + 1) if n in waivers and n not in starts), None)
        if at is None:
            found.append((first, f"{msg}; fix it, or waive it with `# {marker} <reason>`"))
        else:
            claims.setdefault(at, []).append((first, msg))
    for at, owners in claims.items():
        used.add(at)
        if len(owners) > 1:
            found += [(first, f"{msg}; the `# {marker}` waiver on line {at} is claimed by {len(owners)} findings, "
                              "so it excuses none: give each its own line") for first, msg in owners]
        elif len(waivers[at][0]) < MIN_REASON:
            found.append((at, f"`# {marker}` waiver needs a reason of {MIN_REASON}+ characters"))
    for n in sorted(set(waivers) - used):
        found.append((n, f"stale `# {marker}` waiver: nothing on its line (or, for a comment on its own line, "
                         "the next) for it to waive"))
    return sorted(set(found))


def default_files() -> list[Path]:
    return sorted(HERE.glob("*.py"))


def run(files: list[Path], rule: str) -> int:
    bad = 0
    for path in files:
        for line, msg in check_file(path, rule):
            rel = path.resolve().relative_to(REPO) if path.resolve().is_relative_to(REPO) else path
            print(f"{rel}:{line}: {msg}")
            bad += 1
    return 1 if bad else 0


def control(rule: str) -> int:
    """Every `# expect: flag` line reported, no `# expect: clean` line reported."""
    where = FIXTURES / rule.replace("-", "_")
    files = sorted(where.glob("*.py"))
    if not files:
        print(f"control: no fixtures under {where.relative_to(REPO)}")
        return 1
    wrong, marked = [], 0
    for path in files:
        comments = _comments(path.read_text(encoding="utf-8"))
        flagged = {n for n, _ in check_file(path, rule)}
        for n, raw in sorted(comments.items()):
            want = "flag" if "# expect: flag" in raw else "clean" if "# expect: clean" in raw else None
            if want is None:
                continue
            marked += 1
            if (n in flagged) != (want == "flag"):
                wrong.append(f"{path.relative_to(REPO)}:{n}: expected {want}, got {'flag' if n in flagged else 'clean'}")
        unmarked = sorted(flagged - {n for n, raw in comments.items() if "# expect: " in raw})
        wrong += [f"{path.relative_to(REPO)}:{n}: reported, but the line carries no expect marker" for n in unmarked]
    for w in wrong:
        print(f"control: {w}")
    if not marked:
        print("control: the fixtures mark no lines")
        return 1
    print(f"control ({rule}): {marked - len([w for w in wrong if 'expected' in w])}/{marked} fixture lines judged as marked")
    return 1 if wrong else 0


def main(argv: list[str]) -> int:
    usage = __doc__.strip().splitlines()[-2]
    if argv[:1] != ["--rule"] or len(argv) < 2 or argv[1] not in RULES:
        print(usage, file=sys.stderr)
        return 2
    rule, rest = argv[1], argv[2:]
    if rest[:1] == ["--control"]:
        return control(rule) if len(rest) == 1 else 2
    if any(a.startswith("-") for a in rest):
        print(usage, file=sys.stderr)
        return 2
    return run([Path(a) for a in rest] or default_files(), rule)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
