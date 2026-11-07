#!/usr/bin/env python3
"""Lint Check 34: `current_irql` has exactly one writer, irql_store().

Time-at-DISPATCH accounting and a LIFO transition validator both need to see
every IRQL change, and they cannot while a change can be a bare store. This
refuses a store, a compound assignment, an increment or a taken address of the
field anywhere outside include/kernel/sched/irql_store.h. A plain read, or an
__atomic_load_n directly through its address, is not a write. Any other & on the
field is refused, which includes a bitwise AND.

This guards ordinary code, not a determined author: it reads source text, so a
name assembled by the preprocessor (token pasting, a macro alias, a line splice
inside the identifier) is out of its reach.

--control judges the fixtures below and exits 1 on any misjudgement, so a
detector that goes quiet is an error rather than a clean result.
"""
import argparse
import re
import sys
from pathlib import Path

FIELD = "current_irql"
OWNER = "include/kernel/sched/irql_store.h"
ROOTS = ("src", "include")
SUFFIXES = {".c", ".h", ".asm", ".inc", ".S"}

FIELD_RE = re.compile(r"\b" + FIELD + r"\b")
ASSIGN_RE = re.compile(r"(?:=(?!=)|\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<=|>>=|\+\+|--)")
SPLICE_RE = re.compile(r"\\\r?\n")
OPEN_FOR = {")": "(", "]": "["}


def strip_c(text):
    """Blank C comments and literals, keeping every newline so lines still count."""
    out = []
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "//":
            while i < n and text[i] != "\n":
                i += 1
        elif two == "/*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:end]))
            i = end
        elif text[i] in "\"'":
            quote = text[i]
            i += 1
            while i < n and text[i] != quote and text[i] != "\n":
                i += 2 if text[i] == "\\" else 1
            i += 1
            out.append(" ")
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def strip_asm(text):
    return "\n".join(line.split(";", 1)[0] for line in text.split("\n"))


def base_start(code, pos):
    """Start of the postfix expression whose last member begins at pos.

    Walks back over member access, indexing, calls and parentheses by balancing
    brackets, because the base can be any expression: smp_this_cpu(), cpu_data[0],
    (*pcpu), ((struct per_cpu_data *)p). A regex cannot balance them.
    """
    i = pos
    while True:
        j = i
        while j > 0 and code[j - 1].isspace():
            j -= 1
        if code[max(0, j - 2):j] == "->":
            j -= 2
        elif j > 0 and code[j - 1] == ".":
            j -= 1
        else:
            return i
        while j > 0 and code[j - 1].isspace():
            j -= 1
        while j > 0 and code[j - 1] in OPEN_FOR:
            close = code[j - 1]
            depth, j = 1, j - 1
            while j > 0 and depth:
                j -= 1
                if code[j] == close:
                    depth += 1
                elif code[j] == OPEN_FOR[close]:
                    depth -= 1
            while j > 0 and code[j - 1].isspace():
                j -= 1
        while j > 0 and (code[j - 1].isalnum() or code[j - 1] == "_"):
            j -= 1
        i = j


def prev_code(code, pos):
    """The code before pos with whitespace and enclosing open parens removed."""
    return code[:pos].rstrip(" \t\r\n(").rstrip()


def violations(text, is_asm=False):
    """Line numbers where the field is written or has its address escape."""
    code = strip_asm(text) if is_asm else strip_c(SPLICE_RE.sub(" \n", text))
    found = []
    for m in FIELD_RE.finditer(code):
        line = code.count("\n", 0, m.start()) + 1
        if is_asm:
            found.append(line)
            continue
        after = code[m.end():].lstrip(" \t\r\n)")
        if ASSIGN_RE.match(after):
            found.append(line)
            continue
        before = prev_code(code, base_start(code, m.start()))
        if before.endswith(("++", "--")):
            found.append(line)
        elif before.endswith("&") and not before.endswith("&&"):
            # Unary and binary & are not told apart: telling them apart needs C
            # expression context (a cast's closing paren, `return`), and a bitwise
            # AND on an IRQL level has no valid use, so both are refused.
            if not before[:-1].rstrip().endswith("__atomic_load_n("):
                found.append(line)
    return found


# (source, expected number of flagged lines)
FIXTURES = [
    ("pcpu->current_irql = DISPATCH_LEVEL;", 1),
    ("smp_this_cpu()->current_irql = APC_LEVEL;", 1),
    ("cpu_data[0].current_irql  = PASSIVE_LEVEL;", 1),
    ("pcpu->current_irql\n    = x;", 1),
    ("(pcpu->current_irql) = x;", 1),
    ("pcpu->current_irql += 1;", 1),
    ("pcpu->current_irql++;", 1),
    ("++pcpu->current_irql;", 1),
    ("struct per_cpu_data d = { .current_irql = 2 };", 1),
    ("KIRQL *p = &pcpu->current_irql;", 1),
    ("__atomic_store_n(&pcpu->current_irql, 2, __ATOMIC_RELEASE);", 1),
    ("__atomic_store_n(&smp_this_cpu()->current_irql, 2, __ATOMIC_RELEASE);", 1),
    ("++smp_this_cpu()->current_irql;", 1),
    ("--cpu_data[slot + 1].current_irql;", 1),
    ("KIRQL *p = &smp_this_cpu()->current_irql;", 1),
    ("KIRQL *p = &(*pcpu).current_irql;", 1),
    ("KIRQL *p = &((struct per_cpu_data *)q)->current_irql;", 1),
    ("KIRQL *p = &(pcpu->current_irql);", 1),
    ("pcpu->current_irql \\\n    = 2;", 1),
    ("__atomic_store_n(& \\\n    pcpu->current_irql, 2, 0);", 1),
    ("return pcpu->current_irql;", 0),
    ("x = __atomic_load_n(&smp_this_cpu()->current_irql, __ATOMIC_ACQUIRE);", 0),
    ("return &smp_this_cpu()->current_irql;", 1),
    ("__atomic_store_n((KIRQL *)&smp_this_cpu()->current_irql, 2, 0);", 1),
    ("x = flags & pcpu->current_irql;", 1),
    ("if (a && smp_this_cpu()->current_irql) x();", 0),
    ("i++; x = smp_this_cpu()->current_irql;", 0),
    ("if (pcpu->current_irql == DISPATCH_LEVEL) x();", 0),
    ("if (a && pcpu->current_irql < 2) x();", 0),
    ("if (foo(&a)) bar(pcpu->current_irql);", 0),
    ("KIRQL c = pcpu->current_irql; c = 2;", 0),
    ("__atomic_load_n(&t->current_irql, __ATOMIC_ACQUIRE) == 0;", 0),
    ("/* pcpu->current_irql = 2; */", 0),
    ("// pcpu->current_irql = 2;", 0),
    ('klog(LOG_INFO, "t", "current_irql = %u", v);', 0),
]


def control():
    bad = 0
    for source, want in FIXTURES:
        got = len(violations(source))
        if got != want:
            print(f"control: expected {want}, got {got}: {source!r}")
            bad += 1
    if violations("mov byte [gs:current_irql], 2 ; store", is_asm=True) != [1]:
        print("control: assembly reference was not flagged")
        bad += 1
    if violations("nop ; current_irql", is_asm=True):
        print("control: assembly comment was flagged")
        bad += 1
    return bad


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--control", action="store_true", help="judge the fixtures")
    args = parser.parse_args()
    if args.control:
        return 1 if control() else 0
    root = Path(args.root)
    count = 0
    for top in ROOTS:
        for path in sorted((root / top).rglob("*")):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            rel = path.relative_to(root).as_posix()
            if rel == OWNER:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            if FIELD not in text:
                continue
            for line in violations(text, is_asm=path.suffix in {".asm", ".inc"}):
                print(f"{rel}:{line}: raw write of {FIELD}; use irql_store()")
                count += 1
    return 1 if count else 0


if __name__ == "__main__":
    sys.exit(main())
