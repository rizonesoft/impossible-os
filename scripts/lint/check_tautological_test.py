#!/usr/bin/env python3
"""scripts/lint/check_tautological_test.py -- Check 6 worker.

Replaces the inline awk in scripts/lint.sh Check 6 with a parser that
handles the patterns the awk version missed (Codex review 2026-04-28
A-M1 + C-H1):

  (a) TEST_ASSERT_EQ(LHS, RHS, ...) where whitespace-normalized LHS
      and RHS are string-identical -- includes multiline and inner-
      comma cases.
  (b) TEST_ASSERT(true, ...) / TEST_ASSERT(1, ...).
  (c) TEST_ASSERT_EQ(SYM, NUMERIC_LITERAL) where SYM is an UPPERCASE
      identifier whose `#define` body matches NUMERIC_LITERAL exactly.
      The check builds a #define index from include/**/*.h.

Output: one finding per line, format `path:line:detail`.
Allowlist: `TEST-TAUTOLOGY-OK:` substring on the same source line as
the assertion start (lint.sh wraps that).

Usage:
  python3 scripts/lint/check_tautological_test.py <test_file.c> [...]

Returns 0 always (lint.sh interprets the output and counts errors).
"""

import os
import re
import sys
from pathlib import Path
from typing import Dict, List, Tuple


# ---- Define index ----------------------------------------------------------


_DEFINE_RE = re.compile(
    r"^\s*#\s*define\s+([A-Z_][A-Z0-9_]*)\s+([0-9][0-9a-fA-FxXuUlL]*)\s*(?:/\*|//|$)"
)


def _build_define_index(repo_root: Path) -> Dict[str, str]:
    """Walk include/ for `#define UPPERCASE_NAME numeric_literal` lines.
    Returns a dict {NAME: literal-string-as-written}. Numeric-literal
    only; macros with parens, expressions, or string bodies are skipped
    (they're not the symbol-vs-literal-of-symbol pattern this catches).
    """
    idx: Dict[str, str] = {}
    inc = repo_root / "include"
    if not inc.is_dir():
        return idx
    for root, _dirs, files in os.walk(inc):
        for name in files:
            if not name.endswith(".h"):
                continue
            p = Path(root) / name
            try:
                with open(p, "r", encoding="utf-8", errors="replace") as f:
                    for line in f:
                        m = _DEFINE_RE.match(line)
                        if m:
                            sym, lit = m.group(1), m.group(2)
                            # Last definition wins (matches CPP semantics).
                            idx[sym] = lit
            except Exception:
                continue
    return idx


def _normalize_literal(s: str) -> int:
    """Normalize a numeric literal (decimal / hex / suffixed) to an int.
    Returns -1 if the string is not a recognizable integer literal.
    """
    s = s.strip().rstrip("uUlL")
    if not s:
        return -1
    try:
        if s.lower().startswith("0x"):
            return int(s, 16)
        if s.startswith("0") and len(s) > 1 and s[1].isdigit():
            return int(s, 8)
        return int(s)
    except ValueError:
        return -1


# ---- Multiline call accumulator -------------------------------------------


_CALL_START_RE = re.compile(
    r"\b(TEST_ASSERT_EQ|TEST_ASSERT)\s*\("
)


def _accumulate_calls(src: str):
    """Yield (start_line, name, args_text) tuples for every TEST_ASSERT* call.

    Walks the source character by character, ignoring string literals,
    char literals, line comments, and block comments. When a TEST_ASSERT*
    name is encountered followed by `(`, accumulate text until the
    matching close paren. start_line is the 1-based line of the call's
    opening identifier.
    """
    n = len(src)
    i = 0
    line = 1
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        # Line comment: skip to EOL.
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        # Block comment: skip to */.
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                if src[i] == "\n":
                    line += 1
                i += 1
            i += 2
            continue
        # String literal: skip until closing quote (handle escapes).
        if c == '"':
            i += 1
            while i < n and src[i] != '"':
                if src[i] == "\\" and i + 1 < n:
                    if src[i + 1] == "\n":
                        line += 1
                    i += 2
                    continue
                if src[i] == "\n":
                    line += 1
                i += 1
            i += 1
            continue
        # Char literal: skip until closing quote.
        if c == "'":
            i += 1
            while i < n and src[i] != "'":
                if src[i] == "\\" and i + 1 < n:
                    i += 2
                    continue
                i += 1
            i += 1
            continue
        # Look for a call start at this position.
        m = _CALL_START_RE.match(src, i)
        if m:
            name = m.group(1)
            start_line = line
            # Advance past `(`.
            j = m.end()
            depth = 1
            buf = []
            while j < n and depth > 0:
                ch = src[j]
                if ch == "\n":
                    line += 1
                    buf.append(ch)
                    j += 1
                    continue
                if ch == "/" and j + 1 < n and src[j + 1] == "/":
                    while j < n and src[j] != "\n":
                        j += 1
                    continue
                if ch == "/" and j + 1 < n and src[j + 1] == "*":
                    j += 2
                    while j + 1 < n and not (src[j] == "*" and src[j + 1] == "/"):
                        if src[j] == "\n":
                            line += 1
                        j += 1
                    j += 2
                    continue
                if ch == '"':
                    buf.append(ch)
                    j += 1
                    while j < n and src[j] != '"':
                        if src[j] == "\\" and j + 1 < n:
                            buf.append(src[j])
                            buf.append(src[j + 1])
                            j += 2
                            continue
                        if src[j] == "\n":
                            line += 1
                        buf.append(src[j])
                        j += 1
                    if j < n:
                        buf.append(src[j])
                        j += 1
                    continue
                if ch == "'":
                    buf.append(ch)
                    j += 1
                    while j < n and src[j] != "'":
                        if src[j] == "\\" and j + 1 < n:
                            buf.append(src[j])
                            buf.append(src[j + 1])
                            j += 2
                            continue
                        buf.append(src[j])
                        j += 1
                    if j < n:
                        buf.append(src[j])
                        j += 1
                    continue
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                    if depth == 0:
                        j += 1
                        break
                buf.append(ch)
                j += 1
            yield (start_line, name, "".join(buf))
            i = j
            continue
        i += 1


def _split_top_level_commas(s: str) -> List[str]:
    """Split s on commas at the top paren/string-nesting level."""
    parts: List[str] = []
    cur: List[str] = []
    depth = 0
    in_str = False
    in_chr = False
    n = len(s)
    i = 0
    while i < n:
        c = s[i]
        if in_str:
            cur.append(c)
            if c == "\\" and i + 1 < n:
                cur.append(s[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if in_chr:
            cur.append(c)
            if c == "\\" and i + 1 < n:
                cur.append(s[i + 1])
                i += 2
                continue
            if c == "'":
                in_chr = False
            i += 1
            continue
        if c == '"':
            in_str = True
            cur.append(c)
            i += 1
            continue
        if c == "'":
            in_chr = True
            cur.append(c)
            i += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    parts.append("".join(cur))
    return parts


_WS_RE = re.compile(r"\s+")


def _normalize_arg(s: str) -> str:
    """Strip whitespace including newlines for arg comparison."""
    return _WS_RE.sub("", s).strip()


# ---- Source line lookup for marker check -----------------------------------


def _line_text(lines: List[str], n: int) -> str:
    if 1 <= n <= len(lines):
        return lines[n - 1]
    return ""


def _has_allowlist_marker(lines: List[str], start_line: int, end_line: int) -> bool:
    """Allow the call if any line in [start_line, end_line] carries the
    TEST-TAUTOLOGY-OK marker."""
    for n in range(start_line, end_line + 1):
        if "TEST-TAUTOLOGY-OK:" in _line_text(lines, n):
            return True
    return False


# ---- Main ------------------------------------------------------------------


def _check_file(path: Path, defines: Dict[str, str]) -> List[Tuple[int, str]]:
    findings: List[Tuple[int, str]] = []
    try:
        src = open(path, "r", encoding="utf-8", errors="replace").read()
    except Exception:
        return findings
    lines = src.splitlines()
    for start_line, name, args_text in _accumulate_calls(src):
        # Compute end_line by counting newlines in args_text.
        end_line = start_line + args_text.count("\n")
        if _has_allowlist_marker(lines, start_line, end_line):
            continue
        args = _split_top_level_commas(args_text)
        if name == "TEST_ASSERT_EQ" and len(args) >= 2:
            lhs = _normalize_arg(args[0])
            rhs = _normalize_arg(args[1])
            if lhs and rhs and lhs == rhs:
                findings.append(
                    (start_line,
                     f"tautological-test:TEST_ASSERT_EQ identical args ({lhs[:60]})")
                )
                continue
            # (c) symbol-vs-literal-of-symbol
            sym = None
            lit = None
            if re.fullmatch(r"[A-Z_][A-Z0-9_]*", lhs) and re.fullmatch(
                r"[0-9][0-9a-fA-FxXuUlL]*", rhs
            ):
                sym, lit = lhs, rhs
            elif re.fullmatch(r"[A-Z_][A-Z0-9_]*", rhs) and re.fullmatch(
                r"[0-9][0-9a-fA-FxXuUlL]*", lhs
            ):
                sym, lit = rhs, lhs
            if sym and lit:
                define_lit = defines.get(sym)
                if define_lit is not None:
                    a = _normalize_literal(lit)
                    b = _normalize_literal(define_lit)
                    if a >= 0 and b >= 0 and a == b:
                        findings.append(
                            (start_line,
                             f"tautological-test:TEST_ASSERT_EQ({sym}, {lit}) "
                             f"compares define to its own value")
                        )
        elif name == "TEST_ASSERT" and len(args) >= 1:
            first = _normalize_arg(args[0])
            if first in ("true", "1"):
                findings.append(
                    (start_line,
                     "tautological-test:TEST_ASSERT(true|1, ...) is a no-op assertion")
                )
    return findings


def main(argv: List[str]) -> int:
    if len(argv) < 2:
        return 0
    # Repo root: walk up from the first input until include/ is found,
    # else fall back to git rev-parse.
    repo_root = None
    first = Path(argv[1]).resolve()
    cur = first.parent
    while cur != cur.parent:
        if (cur / "include").is_dir() and (cur / ".git").exists():
            repo_root = cur
            break
        cur = cur.parent
    if repo_root is None:
        try:
            import subprocess
            repo_root = Path(
                subprocess.check_output(
                    ["git", "rev-parse", "--show-toplevel"],
                    text=True, timeout=2, stderr=subprocess.DEVNULL,
                ).strip()
            )
        except Exception:
            repo_root = first.parent

    defines = _build_define_index(repo_root)

    for arg in argv[1:]:
        path = Path(arg)
        if not path.is_file():
            continue
        try:
            rel = path.resolve().relative_to(repo_root)
        except Exception:
            rel = path
        for line, detail in _check_file(path, defines):
            print(f"{rel}:{line}:{detail}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
