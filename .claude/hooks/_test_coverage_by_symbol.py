#!/usr/bin/env python3
"""Which tests cover a source: the tests that use what it defines.

The section-commit gate's step-8 rule once bound `src/kernel/x/foo.c` to a test named
`test_foo*.c`. Suites here are registered per subsystem (`test_sched.c` holds the DPC
tests, `test_exec.c` the EIF ones), so 434 of 516 kernel sources had no test of that
name and 23 kernel section commits opted out of the rule though each staged its test.

A test covers a source when code the runners start (test_runner.c's calls into the
file, and the suites those register with test_suite_register_cat) reaches a use of a
public symbol the source defines: a non-static C function, a `#define` or
function in a header, a `global` label in assembly. A source
whose code no test can name (a flat binary such as the AP trampoline) says which test
covers it with a `tested-by: test_<name>.c` line; without one it is untested, never
covered by whichever test happens to be staged beside it.

Upstream ACPICA under src/kernel/acpica/ is not ours to test, except the files
src/libs/PROVENANCE.md lists under "Local additions".

    python3 .claude/hooks/_test_coverage_by_symbol.py --report   # every own source
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

VENDORED_ROOT = "src/kernel/acpica/"
SOURCE_EXTS = (".c", ".asm", ".h")

_IDENT = re.compile(r"\b[A-Za-z_]\w*\b")
# Comments and string or character literals, matched in one pass so a `//` inside a
# string is not a comment and a quote inside a comment is not a string.
_LEXEMES = re.compile(r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", re.S)
_PREPROCESSOR = re.compile(r"^[ \t]*#(?:[^\n]*\\\n)*[^\n]*", re.M)
_ASM_GLOBAL = re.compile(r"^\s*global\s+([A-Za-z_]\w*)", re.M | re.I)
_DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)", re.M)
_TESTED_BY = re.compile(r"tested-by:\s*(test_[\w-]+\.c)\b")
_NAME_THEN_PAREN = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
_NOT_NAMES = frozenset({"if", "while", "for", "switch", "return", "sizeof", "else", "do",
                        "case", "defined", "__attribute__", "_Static_assert", "__declspec",
                        "_Alignas", "alignof", "typeof", "__typeof__", "asm", "__asm__"})


def _code_only(text: str) -> str:
    """C text without comments, string and character literals."""
    return _LEXEMES.sub(lambda m: "\n" * m.group(0).count("\n") or " ", text)


_COND = re.compile(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif|define|undef)\b(.*)")
# The macros the kernel's test flavour defines: the Makefile's test CFLAGS plus what
# the compiler predefines for --target=x86_64-elf. test_step8_suite_coverage.py checks
# each -D name against the Makefile.
TEST_BUILD_DEFINES = {"KERNEL_TESTS": 1, "CONFIG_SMP": 1, "CONFIG_EXCEPT_TELEMETRY": 1,
                      "BUILD_ALT_BOOT_OFF": 1, "__IMPOSSIBLE_OS__": 1, "__x86_64__": 1}
_UNDEF = object()      # known to be undefined (#undef)
_UNKNOWN = object()    # defined, value not an integer literal
_UNCERTAIN = object()  # changed inside a branch that may or may not compile
_EXPR_TOKEN = re.compile(
    r"\s*(defined\b|[A-Za-z_]\w*|0[xX][0-9a-fA-F]+[uUlL]*|\d+[uUlL]*"
    r"|&&|\|\||==|!=|<=|>=|<<|>>|[()!~<>+\-*/%&|^?:])")
_BINARY = {"||": 1, "&&": 2, "|": 3, "^": 4, "&": 5, "==": 6, "!=": 6, "<": 7, ">": 7,
           "<=": 7, ">=": 7, "<<": 8, ">>": 8, "+": 9, "-": 9, "*": 10, "/": 10, "%": 10}


def _int(text: str):
    text = re.sub(r"[uUlL]+$", "", text.strip())
    try:
        return int(text, 0)
    except ValueError:
        return _UNKNOWN


class _Expr:
    """A C preprocessor #if expression, evaluated with C's precedence and its 0/1
    results. None stands for a value this file cannot know and propagates, except
    where && or || is decided by its known side."""

    def __init__(self, toks: list[str], env: dict):
        self.toks, self.env, self.i = toks, env, 0

    def peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else None

    def take(self):
        t = self.peek()
        if t is None:
            raise ValueError("truncated #if expression")
        self.i += 1
        return t

    def ternary(self):
        cond = self.binary(1)
        if self.peek() != "?":
            return cond
        self.take()
        yes = self.ternary()
        if self.take() != ":":
            raise ValueError("?: without :")
        no = self.ternary()
        return None if cond is None else (yes if cond else no)

    def binary(self, floor: int):
        left = self.unary()
        while (op := self.peek()) in _BINARY and _BINARY[op] >= floor:
            self.take()
            right = self.binary(_BINARY[op] + 1)
            left = self.apply(op, left, right)
        return left

    @staticmethod
    def apply(op, a, b):
        if op == "&&":
            return 0 if 0 in (a, b) else (None if None in (a, b) else 1)
        if op == "||":
            return 1 if (a not in (0, None) or b not in (0, None)) else (None if None in (a, b) else 0)
        if a is None or b is None:
            return None
        if op in ("/", "%") and b == 0:
            return None
        return {"|": a | b, "^": a ^ b, "&": a & b, "==": int(a == b), "!=": int(a != b),
                "<": int(a < b), ">": int(a > b), "<=": int(a <= b), ">=": int(a >= b),
                "<<": a << b if 0 <= b < 64 else None, ">>": a >> b if 0 <= b < 64 else None,
                "+": a + b, "-": a - b, "*": a * b,
                "/": int(a / b) if b else None, "%": a - b * int(a / b) if b else None}[op]

    def unary(self):
        t = self.take()
        if t in ("!", "~", "-", "+"):
            v = self.unary()
            return None if v is None else {"!": int(not v), "~": ~v, "-": -v, "+": v}[t]
        if t == "(":
            v = self.ternary()
            if self.take() != ")":
                raise ValueError("unbalanced ( in #if")
            return v
        if t == "defined":
            paren = self.peek() == "("
            if paren:
                self.take()
            name = self.take()
            if paren and self.take() != ")":
                raise ValueError("defined( without )")
            v = self.env.get(name, _UNDEF)
            return None if v is _UNCERTAIN else int(v is not _UNDEF)
        if t[0].isdigit():
            v = _int(t)
            return None if v is _UNKNOWN else v
        v = self.env.get(t)
        if v is _UNDEF:
            return 0  # C: an identifier that is not a macro is 0
        if v is None or v is _UNKNOWN or v is _UNCERTAIN:
            return None  # a header's macro, or a value this file cannot know
        return v


def _condition(kind: str, expr: str, env: dict) -> bool | None:
    """The value of an #if/#ifdef/#ifndef/#elif condition for the test build, or None
    when it depends on what this file cannot see. A name the file never defines is
    undefined for defined()/#ifdef, as C reads it, so include guards and a file's own
    flags evaluate; a header macro used for its value is unknown."""
    expr = _code_only(expr).strip()
    if kind in ("ifdef", "ifndef"):
        name = expr.split()[0] if expr.split() else ""
        v = env.get(name, _UNDEF)
        if v is _UNCERTAIN:
            return None
        return (v is not _UNDEF) == (kind == "ifdef")
    toks, pos = [], 0
    while pos < len(expr):
        m = _EXPR_TOKEN.match(expr, pos)
        if not m:
            if expr[pos:].strip():
                return None
            break
        toks.append(m.group(1))
        pos = m.end()
    try:
        parser = _Expr(toks, env)
        value = parser.ternary()
        if parser.peek() is not None:
            return None
    except (ValueError, KeyError):
        return None
    return None if value is None else bool(value)


def _first_branches(code: str) -> str:
    """`code` without preprocessor lines, keeping the #if branches the test build
    compiles. Each branch is on (compiles), off (does not) or maybe (depends on what
    this file cannot see). Only code under no off and no maybe branch is kept, so code
    that may not compile never earns credit. #define and #undef update the macros
    where they compile, and make the name uncertain where they only may."""
    env = dict(TEST_BUILD_DEFINES)
    out: list[str] = []
    frames: list[dict] = []  # state: "on" | "off" | "maybe"; taken; unknown

    def region():
        states = {f["state"] for f in frames}
        return "off" if "off" in states else ("maybe" if "maybe" in states else "on")

    def state_for(c, taken, unknown):
        if taken or c is False:
            return "off"
        if unknown or c is None:
            return "maybe"
        return "on"

    lines = code.split("\n")
    n = 0
    while n < len(lines):
        line = lines[n]
        if not line.lstrip().startswith("#"):
            out.append(line if region() == "on" else "")
            n += 1
            continue
        full = line
        out.append("")
        while full.rstrip().endswith("\\") and n + 1 < len(lines):
            n += 1
            full = full.rstrip()[:-1] + " " + lines[n]
            out.append("")
        n += 1
        m = _COND.match(full)
        if not m:
            continue
        kind, expr = m.group(1), m.group(2)
        where = region()
        if kind in ("if", "ifdef", "ifndef"):
            if where == "off":
                frames.append({"state": "off", "taken": True, "unknown": False})
                continue
            c = _condition(kind, expr, env)
            frames.append({"state": state_for(c, False, False), "taken": c is True, "unknown": c is None})
        elif kind == "elif" and frames:
            f = frames[-1]
            c = None if f["taken"] else _condition("if", expr, env)
            f["state"] = state_for(c, f["taken"], f["unknown"])
            f["taken"] = f["taken"] or (c is True and not f["unknown"])
            f["unknown"] = f["unknown"] or (c is None and not f["taken"])
        elif kind == "else" and frames:
            f = frames[-1]
            f["state"] = state_for(True, f["taken"], f["unknown"])
            f["taken"] = True
        elif kind == "endif" and frames:
            frames.pop()
        elif kind in ("define", "undef") and where != "off":
            dm = re.match(r"\s*([A-Za-z_]\w*)(\([^)]*\))?\s*(.*)", expr)
            if not dm:
                continue
            name = dm.group(1)
            if where == "maybe":
                env[name] = _UNCERTAIN
            elif kind == "undef":
                env[name] = _UNDEF
            elif dm.group(2):
                env[name] = _UNKNOWN
            else:
                env[name] = _int(dm.group(3)) if dm.group(3).strip() else 1
    return "\n".join(out)


def _close(code: str, i: int, open_: str = "(", close: str = ")") -> int:
    """Index just past the bracket matching the one at `i`, or -1."""
    depth = 0
    for j in range(i, len(code)):
        if code[j] == open_:
            depth += 1
        elif code[j] == close:
            depth -= 1
            if depth == 0:
                return j + 1
    return -1


def _skip_space(code: str, k: int) -> int:
    while k < len(code) and code[k].isspace():
        k += 1
    return k


def _file_scope(code: str):
    """Each `name(...)` declared or defined at file scope, as
    (name, prefix, kind, body) where kind is "def" (a body follows), "decl" (`;` or
    `,` follows) or "other", prefix is the declaration text since the previous `;` or
    `}`, and body is the function body text for a "def". Bodies are skipped whole.
    """
    start = 0
    i = 0
    n = len(code)
    while i < n:
        c = code[i]
        if c == "{":
            end = _close(code, i, "{", "}")
            if end < 0:
                return
            i = start = end  # a struct, enum or initialiser body at file scope
            continue
        if c == ";":
            start = i + 1
        elif (c.isalpha() or c == "_") and (i == 0 or not (code[i - 1].isalnum() or code[i - 1] == "_")):
            m = _NAME_THEN_PAREN.match(code, i)
            if m and m.group(1) not in _NOT_NAMES:
                end = _close(code, m.end() - 1)
                if end < 0:
                    return
                k = _skip_space(code, end)
                while code.startswith("__attribute__", k):
                    k = _close(code, code.find("(", k))
                    if k < 0:
                        return
                    k = _skip_space(code, k)
                follows = code[k] if k < n else ""
                prefix = code[start:i]
                if follows == "{":
                    body_end = _close(code, k, "{", "}")
                    if body_end < 0:
                        return
                    yield m.group(1), prefix, "def", code[k:body_end]
                    i = start = body_end
                    continue
                yield m.group(1), prefix, ("decl" if follows in ";," else "other"), ""
                i = m.end()
                continue
            while i < n and (code[i].isalnum() or code[i] == "_"):
                i += 1
            continue
        i += 1


def _top_level_functions(code: str, declarations: bool) -> set[str]:
    """Non-static functions defined (or, with `declarations`, declared) at file scope."""
    names = set()
    for name, prefix, kind, _ in _file_scope(code):
        typed = prefix.strip() != "" and "=" not in prefix
        if typed and (kind == "def" or (declarations and kind == "decl")) \
                and not re.search(r"\b(static|typedef)\b", prefix):
            names.add(name)
    return names


def local_additions(provenance_text: str) -> set[str] | None:
    """The paths PROVENANCE.md lists under its "Local additions" heading, or None when
    the heading is missing (the vendored/ours split is then unknown)."""
    m = re.search(r"^## Local additions.*?$(.*?)(?=^## |\Z)", provenance_text, re.M | re.S)
    if not m:
        return None
    return {p for p in re.findall(r"`([^`\s]+)`", m.group(1)) if p.startswith("src/")}


def is_vendored(path: str, local: set[str]) -> bool:
    return path.startswith(VENDORED_ROOT) and path not in local


def public_symbols(path: str, text: str) -> set[str]:
    """What a test could name to exercise `path`."""
    if path.endswith(".asm"):
        return set(_ASM_GLOBAL.findall(text))
    code = _code_only(text)
    header = path.endswith(".h")
    names = set(_DEFINE.findall(code)) if header else set()
    names |= _top_level_functions(_first_branches(code), declarations=header)
    if header:  # a header's static inline helpers are its interface too
        names |= {m.group(1) for m in re.finditer(r"\bstatic\s+inline\b[^;{(]*?\b([A-Za-z_]\w*)\s*\(", code)}
    return names - _NOT_NAMES


_REGISTER = re.compile(r"\btest_suite_register(?:_cat)?\s*\(")
_KEYWORDS = frozenset({"return", "else", "case", "goto", "sizeof", "do", "if", "while", "for", "switch"})
# `type name =`, `type *name;`, `type name[`: a local the body declares. The type is
# one or more words, so `return x;` (a keyword before the name) is not one.
_LOCAL_DECL = re.compile(r"\b([A-Za-z_]\w*)[\s\*]+([A-Za-z_]\w*)\s*(?=[=;\[,)])")
# `int f(int);` inside a body: a prototype, not a call.
_LOCAL_PROTO = re.compile(r"\b([A-Za-z_]\w*)[\s\*]+([A-Za-z_]\w*)\s*\([^;{}()]*\)\s*;")


def _body_uses(body: str) -> set[str]:
    """The identifiers a function body uses, less the locals it declares. A block-scope
    prototype is dropped as text: it is not a use, but it does not shadow the name."""
    # A prototype names the real function: drop only its text. `return f(x);` is a call.
    body = _LOCAL_PROTO.sub(lambda m: m.group(0) if m.group(1) in _KEYWORDS else " ", body)
    declared = {m.group(2) for m in _LOCAL_DECL.finditer(body) if m.group(1) not in _KEYWORDS}
    return set(_IDENT.findall(body)) - declared


def _bodies(text: str) -> tuple[dict[str, str], set[str]]:
    """{function: body} for every function the test build compiles in `text`, and the
    names of the non-static ones."""
    bodies: dict[str, str] = {}
    exported: set[str] = set()
    for name, prefix, kind, body in _file_scope(_first_branches(_code_only(text))):
        if kind == "def":
            bodies[name] = bodies.get(name, "") + body
            if not re.search(r"\bstatic\b", prefix):
                exported.add(name)
    return bodies, exported


def test_names(text: str, entry_points: frozenset[str] = frozenset()) -> set[str]:
    """The identifiers the test's executed code uses. Execution starts at this file's
    non-static functions a runner calls (`entry_points`, from runner_entry_points) and
    follows the calls in their bodies; a test_suite_register(_cat) call in a reached
    body adds its callback. A declaration, a comment, a string, a local variable, code
    the test build does not compile, or a function nothing reaches is not a use.
    """
    bodies, exported = _bodies(text)
    seen: set[str] = set()
    used: set[str] = set()
    todo = list(exported & entry_points)
    while todo:
        fn = todo.pop()
        if fn in seen:
            continue
        seen.add(fn)
        body = bodies[fn]
        ids = _body_uses(body)
        for m in _REGISTER.finditer(body):
            end = _close(body, m.end() - 1)
            ids |= set(_IDENT.findall(body[m.end():end if end > 0 else len(body)]))
        used |= ids
        todo.extend(i for i in ids if i in bodies and i not in seen)
    return used


RUNNERS = ("src/kernel/test/test_runner.c", "src/kernel/main/boot_tests.c")


def runner_entry_points(runner_texts) -> frozenset[str]:
    """What the runners run: starting from their non-static functions (what the boot
    path calls into), the uses in every runner function those reach. A declaration, a
    comment, a call under `#if 0` or an uncalled static helper starts nothing."""
    bodies: dict[str, str] = {}
    exported: set[str] = set()
    for text in runner_texts:
        if text:
            b, e = _bodies(text)
            for name, body in b.items():
                bodies[name] = bodies.get(name, "") + body
            exported |= e
    seen: set[str] = set()
    names: set[str] = set()
    todo = list(exported)
    while todo:
        fn = todo.pop()
        if fn in seen:
            continue
        seen.add(fn)
        uses = _body_uses(bodies[fn])
        names |= uses
        todo.extend(u for u in uses if u in bodies and u not in seen)
    return frozenset(names)


def covering_tests(path: str, text: str, tests: dict[str, set[str]]) -> list[str]:
    """The tests in `tests` ({path: test_names}) that cover `path`."""
    named = set(_TESTED_BY.findall(text))
    symbols = public_symbols(path, text)
    return sorted(t for t, used in tests.items()
                  if Path(t).name in named or symbols & used)


def _git(root: Path, *args: str) -> str:
    return subprocess.run(["git", *args], cwd=str(root), capture_output=True, text=True,
                          timeout=30, check=True).stdout  # deadline: 30 s, a local read


def report(root: Path) -> tuple[dict[str, list[str]], list[str]]:
    """Every own kernel source at HEAD: {source: covering tests}, and the untested ones."""
    local = local_additions(_git(root, "show", "HEAD:src/libs/PROVENANCE.md"))
    if local is None:
        raise SystemExit("src/libs/PROVENANCE.md has no Local additions heading")
    files = _git(root, "ls-tree", "-r", "--name-only", "HEAD", "src/kernel").split()
    entry = runner_entry_points((root / r).read_text(errors="replace") for r in RUNNERS)
    tests = {f: test_names((root / f).read_text(errors="replace"), entry) for f in files
             if re.search(r"/test/test_[\w-]+\.c$", f)}
    covered: dict[str, list[str]] = {}
    untested: list[str] = []
    for f in files:
        if "/test/" in f or not f.endswith(SOURCE_EXTS) or is_vendored(f, local):
            continue
        hits = covering_tests(f, (root / f).read_text(errors="replace"), tests)
        if hits:
            covered[f] = hits
        else:
            untested.append(f)
    return covered, untested


def main(argv: list[str]) -> int:
    if argv[1:] != ["--report"]:
        print(__doc__.strip().splitlines()[-1].strip())
        return 2
    root = Path(_git(Path.cwd(), "rev-parse", "--show-toplevel").strip())
    covered, untested = report(root)
    print(f"{len(covered)} covered, {len(untested)} untested "
          f"(own src/kernel sources at HEAD; upstream ACPICA excluded)")
    for f, tests in covered.items():
        print(f"covered   {f}  <- {', '.join(Path(t).name for t in tests)}")
    for f in untested:
        print(f"untested  {f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
