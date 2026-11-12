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
_UNDEF = object()   # known to be undefined (#undef)
_UNKNOWN = object()  # defined, value not an integer
_EXPR_TOKEN = re.compile(r"\s*(defined|[A-Za-z_]\w*|0[xX][0-9a-fA-F]+|\d+|&&|\|\||==|!=|<=|>=|[()!<>+\-*])")


def _int(text: str):
    text = re.sub(r"[uUlL]+$", "", text.strip())
    try:
        return int(text, 0)
    except ValueError:
        return _UNKNOWN


def _condition(kind: str, expr: str, env: dict) -> bool | None:
    """The value of an #if/#ifdef/#ifndef/#elif condition for the test build, or None
    when it depends on a macro whose value is not known here (one from a header).
    A name never defined in `env` is undefined, as C reads it, so include guards and a
    file's own flags evaluate; a header macro used for its VALUE is unknown."""
    expr = _code_only(expr).strip()
    if kind in ("ifdef", "ifndef"):
        name = expr.split()[0] if expr.split() else ""
        defined = name in env and env[name] is not _UNDEF
        return defined == (kind == "ifdef")
    out = []
    pos = 0
    toks = []
    while pos < len(expr):
        m = _EXPR_TOKEN.match(expr, pos)
        if not m:
            return None
        toks.append(m.group(1))
        pos = m.end()
    i = 0
    while i < len(toks):
        t = toks[i]
        if t == "defined":
            j = i + 1
            paren = j < len(toks) and toks[j] == "("
            j += paren
            if j >= len(toks):
                return None
            name = toks[j]
            out.append("1" if name in env and env[name] is not _UNDEF else "0")
            i = j + 1 + (paren and j + 1 < len(toks) and toks[j + 1] == ")")
            continue
        if t[0].isalpha() or t[0] == "_":
            v = env.get(t, _UNDEF if t not in env else env[t])
            if t not in env or v is _UNDEF:
                return None  # a value from a header this file cannot see
            if v is _UNKNOWN:
                return None
            out.append(str(v))
        elif t[0].isdigit():
            v = _int(t)
            if v is _UNKNOWN:
                return None
            out.append(str(v))
        else:
            out.append({"&&": " and ", "||": " or ", "!": " not "}.get(t, t))
        i += 1
    try:
        return bool(eval("".join(out) or "0", {"__builtins__": {}}, {}))  # noqa: S307 (tokens are ints and operators only)
    except Exception:
        return None


def _first_branches(code: str) -> str:
    """`code` without preprocessor lines, keeping the #if branches the test build
    compiles. A branch whose condition is unknown is dropped, and so is every later
    branch of its chain, because whether they compile is unknown too: disabled code
    must never earn credit. #define and #undef in compiled regions update the macros."""
    env = dict(TEST_BUILD_DEFINES)
    out = []
    stack: list[list] = []  # per open #if: [this branch compiled, a branch taken, unknown seen]
    lines = code.split("\n")
    n = 0
    while n < len(lines):
        line = lines[n]
        if not line.lstrip().startswith("#"):
            out.append(line if all(f[0] for f in stack) else "")
            n += 1
            continue
        full = line
        while full.rstrip().endswith("\\") and n + 1 < len(lines):
            n += 1
            full = full.rstrip()[:-1] + " " + lines[n]
            out.append("")
        out.append("")
        n += 1
        m = _COND.match(full)
        if not m:
            continue
        kind, expr = m.group(1), m.group(2)
        live = all(f[0] for f in stack)
        if kind in ("if", "ifdef", "ifndef"):
            c = _condition(kind, expr, env) if live else False
            stack.append([c is True, c is True, c is None])
        elif kind == "elif" and stack:
            f = stack[-1]
            outer = all(g[0] for g in stack[:-1])
            if f[1] or f[2] or not outer:
                f[0] = False
            else:
                c = _condition("if", expr, env)
                f[0] = c is True
                f[1] = c is True
                f[2] = c is None
        elif kind == "else" and stack:
            f = stack[-1]
            f[0] = not f[1] and not f[2] and all(g[0] for g in stack[:-1])
            f[1] = True
        elif kind == "endif" and stack:
            stack.pop()
        elif kind == "define" and live:
            dm = re.match(r"\s*([A-Za-z_]\w*)(\([^)]*\))?\s*(.*)", expr)
            if dm:
                env[dm.group(1)] = _UNKNOWN if dm.group(2) else (_int(dm.group(3)) if dm.group(3).strip() else 1)
        elif kind == "undef" and live:
            name = expr.split()[0] if expr.split() else ""
            env[name] = _UNDEF
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
    """What the runners call: the uses in the bodies of their compiled functions, so a
    declaration, a comment or a call under `#if 0` starts nothing."""
    names: set[str] = set()
    for text in runner_texts:
        if text:
            for body in _bodies(text)[0].values():
                names |= _body_uses(body)
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
