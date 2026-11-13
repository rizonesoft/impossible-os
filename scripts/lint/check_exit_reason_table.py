#!/usr/bin/env python3
"""Lint Check 39: a reserved exit reason is declared only in TASK_EXIT_REASONS.

include/kernel/sched/task.h generates every reserved exit reason, its count,
_LAST and the classifier bounds from one X-macro table. A reason declared
anywhere else (a `#define TASK_EXIT_X (TASK_EXIT_REASON_LAST - 1)`, an enum
or const) builds clean and classifies as an ordinary application status,
which is the defect the table exists to make impossible.

The table therefore defines the WHOLE `TASK_EXIT_*` namespace: one name per
row plus the table's own machinery. Any other `TASK_EXIT_*` identifier in the
code, declared or used, in any declaration form, is a finding. No declaration
syntax is parsed, so attributes, parenthesised declarators and implicit
enumerators need no special case.

Identifiers are read after C translation phases 2 and 3 (splices removed,
then each comment one space, literals emptied), so a comment or string that
mentions a name is not code and a name split by a continuation is still found.
A name BUILT by token pasting is out of scope; the table's own row asserts and
review own that.

    check_exit_reason_table.py --root <repo>   exit 1 on any such declaration
    check_exit_reason_table.py --control       exit 1 unless the built-in
                                               fixture is judged as marked
    check_exit_reason_table.py <file>...       check these files only
"""
import os
import re
import sys

# The table's own machinery: the base, the table, its count and _LAST, the X
# helpers and the row-position enum prefix the table pastes.
MACHINERY = {
    "TASK_EXIT_REASON_BASE", "TASK_EXIT_REASONS", "TASK_EXIT_REASON_LAST",
    "TASK_EXIT_REASON_COUNT", "TASK_EXIT_REASON_CONST", "TASK_EXIT_REASON_ROW",
    "TASK_EXIT_REASON_ROW_", "TASK_EXIT_REASON_ORD_CHECK", "TASK_EXIT_REASON_NAME",
    "TASK_EXIT_",  # the prefix TASK_EXIT_##name pastes each row name onto
}
TABLE_H = os.path.join("include", "kernel", "sched", "task.h")
IDENT = re.compile(r"\bTASK_EXIT_[A-Za-z0-9_]*")
# Matched on TRANSLATED text, where the table's splices are gone and a
# commented-out row is a space: only a row the compiler sees counts.
TABLE = re.compile(r"#[ \t]*define[ \t]+TASK_EXIT_REASONS[ \t]*\(X\)([^\n]*)")
ROW = re.compile(r"\bX\(\s*([A-Za-z0-9_]+)\s*,")


def translate(text):
    """Apply C translation phases 2 and 3 as far as this check needs them.

    Phase 2 deletes every backslash-newline, so a splice can fall inside a
    directive name or an identifier (`#defi\\<newline>ne`). Phase 3 then turns
    each comment into ONE space, newlines included, so a comment cannot end a
    directive line. String and character literals are copied through, so a
    comment marker inside one is not a comment. Returns the translated text and,
    for each of its characters, the 1-based source line it came from.
    """
    spliced, lines, line = [], [], 1
    i, n = 0, len(text)
    while i < n:
        if text[i] == "\\" and i + 1 < n and text[i + 1] == "\n":
            i += 2
            line += 1
            continue
        spliced.append(text[i])
        lines.append(line)
        if text[i] == "\n":
            line += 1
        i += 1
    src = "".join(spliced)
    out, origin = [], []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("/*", i):
            end = src.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append(" ")
            origin.append(lines[i])
            i = end
        elif src.startswith("//", i):
            end = src.find("\n", i)
            end = n if end < 0 else end
            out.append(" ")
            origin.append(lines[i])
            i = end
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            # The contents never declare anything: keep an empty literal.
            out.append(c + c)
            origin.extend((lines[i], lines[j - 1]))
            i = j
        else:
            out.append(c)
            origin.append(lines[i])
            i += 1
    return "".join(out), origin


def table_names(text):
    """The names the TASK_EXIT_REASONS table defines, or None if it is absent."""
    m = TABLE.search(translate(text)[0])
    if not m:
        return None
    return {"TASK_EXIT_" + r for r in ROW.findall(m.group(1))}


def findings(text, path, allowed):
    # Fast rejection: no file without the prefix (after splices) can name one.
    if "TASK_EXIT_" not in text.replace("\\\n", ""):
        return []
    body, origin = translate(text)
    out = []
    for m in IDENT.finditer(body):
        if m.group(0) in allowed:
            continue
        out.append(f"{path}:{origin[m.start()]}: {m.group(0)} is not a TASK_EXIT_REASONS row "
                   f"(add it as a row in {TABLE_H})")
    return out


# A table, then one name that is not in it per bypass shape: a #define and
# enumerators in any layout, an attributed and a parenthesised declarator, an
# implicit enumerator, a use with no declaration, a splice inside the name,
# and a name whose only row is commented out.
CONTROL_TABLE = """\
#define TASK_EXIT_REASONS(X) \\
    X(KNOWN_A, 1) \\
    X(KNOWN_B, 2) \\
    /* X(UTEST_PROBE9, 3) reserved for later */
"""
CONTROL_BAD = CONTROL_TABLE + """\
#define TASK_EXIT_UTEST_PROBE (TASK_EXIT_REASON_LAST - 1)
enum { TASK_EXIT_UTEST_PROBE2
       = TASK_EXIT_REASON_BASE - 9 };
static const int TASK_EXIT_UTEST_PROBE3 __attribute__((unused)) = -1007;
static const int (TASK_EXIT_UTEST_PROBE4) = -1007;
enum { anchor = TASK_EXIT_REASON_LAST - 2, TASK_EXIT_UTEST_PROBE5 };
#define /* a reason
   spread over lines */ TASK_EXIT_UTEST_PROBE6 (-1006)
if (s == TASK_EXIT_UTEST_PROBE7) return;
#define TASK_EXIT_\\
UTEST_PROBE8 (-1006)
#define TASK_EXIT_UTEST_PROBE9 (TASK_EXIT_REASON_LAST - 1)
"""
CONTROL_BAD_COUNT = 9
CONTROL_GOOD = CONTROL_TABLE + """\
#define TASK_EXIT_REASON_BASE (-1000)
#define TASK_EXIT_REASON_CONST(name, ord) TASK_EXIT_##name = TASK_EXIT_REASON_BASE - (ord),
#define TASK_EXIT_REASON_LAST (TASK_EXIT_REASON_BASE - TASK_EXIT_REASON_COUNT)
if (status == TASK_EXIT_KNOWN_A) t->exit_status = TASK_EXIT_KNOWN_B;
/* TASK_EXIT_ONLY_IN_A_COMMENT */
klog(LOG_INFO, "TASK_EXIT_ONLY_IN_A_STRING");
"""


def tree_files(root):
    for top in ("include", "src", "user"):
        for d, _dirs, files in os.walk(os.path.join(root, top)):
            for f in files:
                if f.endswith((".c", ".h")):
                    yield os.path.join(d, f)


def read(p):
    with open(p, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def main(argv):
    if argv[1:] == ["--control"]:
        allowed = MACHINERY | table_names(CONTROL_TABLE)
        bad = findings(CONTROL_BAD, "control-bad", allowed)
        good = findings(CONTROL_GOOD, "control-good", allowed)
        if len(bad) == CONTROL_BAD_COUNT and not good:
            return 0
        print(f"control misjudged: {len(bad)} of {CONTROL_BAD_COUNT} names flagged, "
              f"{len(good)} false positives: {bad + good}", file=sys.stderr)
        return 1
    if argv[1:2] == ["--root"] and len(argv) == 3:
        root = argv[2]
        paths = list(tree_files(root))
    elif len(argv) > 1 and not argv[1].startswith("-"):
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        paths = argv[1:]
    else:
        print(__doc__, file=sys.stderr)
        return 2
    # The table comes from the files checked when one of them holds it (a
    # mutant header), else from the tree's task.h.
    texts, out = {}, []
    for p in paths:
        try:
            texts[p] = read(p)
        except OSError as e:
            out.append(f"{p}: unreadable: {e}")
    names = next((n for n in map(table_names, texts.values()) if n is not None), None)
    if names is None:
        try:
            names = table_names(read(os.path.join(root, TABLE_H)))
        except OSError:
            names = None
    if not names:
        print(f"no TASK_EXIT_REASONS table found in {TABLE_H}", file=sys.stderr)
        return 1
    allowed = MACHINERY | names
    for p, text in texts.items():
        out += findings(text, p, allowed)
    for line in out:
        print(line)
    return 1 if out else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
