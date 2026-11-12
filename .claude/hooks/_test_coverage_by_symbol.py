#!/usr/bin/env python3
"""Which tests cover a source: the tests that use what it defines.

The section-commit gate's step-8 rule once bound `src/kernel/x/foo.c` to a test named
`test_foo*.c`. Suites here are registered per subsystem (`test_sched.c` holds the DPC
tests, `test_exec.c` the EIF ones), so 434 of 516 kernel sources had no test of that
name and 23 kernel section commits opted out of the rule though each staged its test.

A test covers a source when code it runs (a suite it registers with
test_suite_register_cat, or an entry point a runner calls) reaches a use of a
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


_COND = re.compile(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b(.*)")
# What the kernel's test flavour defines (KERNEL_TESTS; x86-64 target). A condition
# over anything else is unknown.
TEST_BUILD_DEFINES = frozenset({"KERNEL_TESTS", "__x86_64__"})


def _condition(kind: str, expr: str) -> bool | None:
    """The value of an #if/#ifdef/#ifndef/#elif condition, or None when unknown."""
    expr = expr.strip()
    if kind in ("ifdef", "ifndef"):
        name = expr.split()[0] if expr.split() else ""
        return (name in TEST_BUILD_DEFINES) == (kind == "ifdef")
    neg = expr.startswith("!")
    expr = expr.lstrip("!").strip()
    m = re.fullmatch(r"defined\s*\(?\s*([A-Za-z_]\w*)\s*\)?|([A-Za-z_]\w*)|(\d+)", expr)
    if not m:
        return None
    if m.group(3) is not None:
        value = int(m.group(3)) != 0
    elif m.group(1) or m.group(2) in TEST_BUILD_DEFINES:
        value = (m.group(1) or m.group(2)) in TEST_BUILD_DEFINES
    else:
        return None  # a bare macro whose value the test build does not pin
    return value != neg


def _first_branches(code: str) -> str:
    """`code` without preprocessor lines, keeping one branch of each #if chain: the
    first whose condition the test build makes true, else the first unknown one, else
    the #else. Mutually exclusive branches therefore never both open a brace."""
    out = []
    stack: list[list[bool]] = []  # per open #if: [this branch kept, a branch taken]
    cont = False
    for line in code.split("\n"):
        directive = cont or line.lstrip().startswith("#")
        cont = directive and line.rstrip().endswith("\\")
        m = _COND.match(line) if directive else None
        if m:
            kind, expr = m.group(1), m.group(2)
            if kind.startswith("if"):
                keep = _condition(kind, expr) is not False
                stack.append([keep, keep])
            elif stack and kind == "elif":
                keep = not stack[-1][1] and _condition("if", expr) is not False
                stack[-1] = [keep, stack[-1][1] or keep]
            elif stack and kind == "else":
                stack[-1] = [not stack[-1][1], True]
            elif stack and kind == "endif":
                stack.pop()
        out.append("" if directive or not all(k for k, _ in stack) else line)
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


def test_names(text: str, entry_points: frozenset[str] = frozenset()) -> set[str]:
    """The identifiers the test's executed code uses: the bodies of the functions passed
    to test_suite_register(_cat), of this file's non-static functions the runner names
    (`entry_points`, the identifiers of the RUNNERS), and of every function in this
    file those bodies reach. A declaration, a comment, a string, a local variable, or a
    function nothing runs is not a call.
    """
    code = _first_branches(_code_only(text))
    bodies: dict[str, str] = {}
    exported: set[str] = set()
    for name, prefix, kind, body in _file_scope(code):
        if kind == "def":
            bodies[name] = bodies.get(name, "") + body
            if not re.search(r"\bstatic\b", prefix):
                exported.add(name)
    roots = exported & entry_points
    for body in bodies.values():
        for m in _REGISTER.finditer(body):
            end = _close(body, m.end() - 1)
            roots |= set(_IDENT.findall(body[m.end():end if end > 0 else len(body)])) & set(bodies)
    seen: set[str] = set()
    used: set[str] = set()
    todo = list(roots)
    while todo:
        fn = todo.pop()
        if fn in seen:
            continue
        seen.add(fn)
        ids = _body_uses(bodies[fn])
        used |= ids
        todo.extend(i for i in ids if i in bodies and i not in seen)
    return used


# The files that start test code: the suite runner, and the boot path that launches the
# user-mode suite. A test file's non-static function runs only if one of them names it.
RUNNERS = ("src/kernel/test/test_runner.c", "src/kernel/main/boot_tests.c")


def runner_entry_points(runner_texts) -> frozenset[str]:
    """The identifiers the runners' code uses: the entry points they call."""
    return frozenset().union(*(_IDENT.findall(_code_only(t)) for t in runner_texts if t))


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
