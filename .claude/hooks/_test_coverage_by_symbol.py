#!/usr/bin/env python3
"""Which tests cover a source: the tests that use what it defines.

The section-commit gate's step-8 rule once bound `src/kernel/x/foo.c` to a test named
`test_foo*.c`. Suites here are registered per subsystem (`test_sched.c` holds the DPC
tests, `test_exec.c` the EIF ones), so 434 of 516 kernel sources had no test of that
name and 23 kernel section commits opted out of the rule though each staged its test.

A test covers a source when it names a public symbol the source defines: a non-static C
function, a `#define` or function in a header, a `global` label in assembly. A source
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


def _close(code: str, i: int) -> int:
    """Index just past the `)` matching the `(` at `i`, or -1."""
    depth = 0
    for j in range(i, len(code)):
        if code[j] == "(":
            depth += 1
        elif code[j] == ")":
            depth -= 1
            if depth == 0:
                return j + 1
    return -1


def _top_level_functions(code: str, declarations: bool) -> set[str]:
    """Non-static functions named at file scope: each `name(...)` at brace depth 0 that
    a body follows (or, with `declarations`, a `;`). The storage class is read from the
    whole declaration since the previous `;` or `}`, so `static` on the line above counts.
    """
    names: set[str] = set()
    depth = 0
    start = 0  # where the current top-level declaration begins
    i = 0
    n = len(code)
    while i < n:
        c = code[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth = max(depth - 1, 0)
            if depth == 0:
                start = i + 1
        elif c == ";" and depth == 0:
            start = i + 1
        elif depth == 0 and (c.isalpha() or c == "_") and (i == 0 or not (code[i - 1].isalnum() or code[i - 1] == "_")):
            m = _NAME_THEN_PAREN.match(code, i)
            if m and m.group(1) not in _NOT_NAMES:
                end = _close(code, m.end() - 1)
                if end < 0:
                    break
                k = end
                while k < n and code[k].isspace():
                    k += 1
                while code.startswith("__attribute__", k):  # trailing attributes
                    p = code.find("(", k)
                    k = _close(code, p) if p >= 0 else -1
                    if k < 0:
                        return names
                    while k < n and code[k].isspace():
                        k += 1
                follows = code[k] if k < n else ""
                prefix = code[start:i]
                typed = prefix.strip() != "" and "=" not in prefix
                if typed and (follows == "{" or (declarations and follows in ";,")) \
                        and not re.search(r"\b(static|typedef)\b", prefix):
                    names.add(m.group(1))
                i = m.end()
                continue
            while i < n and (code[i].isalnum() or code[i] == "_"):
                i += 1
            continue
        i += 1
    return names


def local_additions(provenance_text: str) -> set[str]:
    """The paths PROVENANCE.md lists under its "Local additions" heading."""
    m = re.search(r"^## Local additions.*?$(.*?)(?=^## |\Z)", provenance_text, re.M | re.S)
    if not m:
        return set()
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
    names |= _top_level_functions(_PREPROCESSOR.sub("", code), declarations=header)
    if header:  # a header's static inline helpers are its interface too
        names |= {m.group(1) for m in re.finditer(r"\bstatic\s+inline\b[^;{(]*?\b([A-Za-z_]\w*)\s*\(", code)}
    return names - _NOT_NAMES


def test_names(text: str) -> set[str]:
    """The identifiers a test's code uses (not its comments or string literals)."""
    return set(_IDENT.findall(_code_only(text)))


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
    files = _git(root, "ls-tree", "-r", "--name-only", "HEAD", "src/kernel").split()
    tests = {f: test_names((root / f).read_text(errors="replace")) for f in files
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
    for f in untested:
        print(f"untested  {f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
