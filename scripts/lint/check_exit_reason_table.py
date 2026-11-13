#!/usr/bin/env python3
"""Lint Check 39: a reserved exit reason is declared only in TASK_EXIT_REASONS.

include/kernel/sched/task.h generates every reserved exit reason, its count,
_LAST and the classifier bounds from one X-macro table. A reason declared
anywhere else (a `#define TASK_EXIT_X (TASK_EXIT_REASON_LAST - 1)`, an enum
or const `TASK_EXIT_X = ...`) builds clean and classifies as an ordinary
application status, which is the defect the table exists to make impossible.

Scope: the guard is against an HONEST declaration in any layout the compiler
accepts: the source is read after C translation phases 2 and 3 (splices
removed, then each comment one space), so whitespace, comments and line
continuations anywhere in it change nothing. A name BUILT by token pasting or
another macro is out of scope; the table's own row asserts and review own that.

    check_exit_reason_table.py --root <repo>   exit 1 on any such declaration
    check_exit_reason_table.py --control       exit 1 unless the built-in
                                               fixture is judged as marked
    check_exit_reason_table.py <file>...       check these files only
"""
import os
import re
import sys

# The table's own machinery: the base, the table, _LAST and the X helpers.
ALLOWED = {
    "TASK_EXIT_REASON_BASE", "TASK_EXIT_REASONS", "TASK_EXIT_REASON_LAST",
    "TASK_EXIT_REASON_CONST", "TASK_EXIT_REASON_ROW", "TASK_EXIT_REASON_ORD_CHECK",
    "TASK_EXIT_REASON_NAME",
}
DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(TASK_EXIT_[A-Z0-9_]+)\b", re.M)
# An enumerator or initialised constant: NAME = value (not ==). Any C
# whitespace, newlines included, may sit before the `=`.
ASSIGN = re.compile(r"\b(TASK_EXIT_[A-Z][A-Z0-9_]*)\s*=(?!=)")


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


def findings(text, path):
    body, origin = translate(text)
    out = []
    for rx, shape in ((DEFINE, "#define"), (ASSIGN, "declaration")):
        for m in rx.finditer(body):
            if m.group(1) in ALLOWED:
                continue
            line = origin[m.start(1)] if origin else 1
            out.append(f"{path}:{line}: {shape} of {m.group(1)} outside TASK_EXIT_REASONS "
                       f"(add it as a table row instead)")
    return out


CONTROL_BAD = """\
/* #define TASK_EXIT_IN_A_COMMENT (TASK_EXIT_REASON_LAST - 1) */
#define TASK_EXIT_UTEST_PROBE (TASK_EXIT_REASON_LAST - 1)
enum { TASK_EXIT_UTEST_PROBE2 = TASK_EXIT_REASON_BASE - 9 };
static const int TASK_EXIT_UTEST_PROBE3 = -1007;
enum { TASK_EXIT_UTEST_PROBE4
       = TASK_EXIT_REASON_LAST - 1 };
#define/**/TASK_EXIT_UTEST_PROBE5 (-1006)
#define \\
    TASK_EXIT_UTEST_PROBE6 (-1006)
#define /* a reason
   spread over lines */ TASK_EXIT_UTEST_PROBE7 (-1006)
#defi\\
ne TASK_EXIT_UTEST_PROBE8 (-1006)
#define TASK_EXIT_\\
UTEST_PROBE9 (-1006)
"""
CONTROL_GOOD = """\
#define TASK_EXIT_REASON_BASE (-1000)
#define TASK_EXIT_REASON_CONST(name, ord) TASK_EXIT_##name = TASK_EXIT_REASON_BASE - (ord),
#define TASK_EXIT_REASON_LAST (TASK_EXIT_REASON_BASE - TASK_EXIT_REASON_COUNT)
if (status == TASK_EXIT_UTEST_TIMEOUT) t->exit_status = TASK_EXIT_UTEST_TIMEOUT;
klog(LOG_INFO, "#define TASK_EXIT_IN_A_STRING (-1006) /* or TASK_EXIT_X = 1 */");
"""


def tree_files(root):
    for top in ("include", "src"):
        for d, _dirs, files in os.walk(os.path.join(root, top)):
            for f in files:
                if f.endswith((".c", ".h")):
                    yield os.path.join(d, f)


def main(argv):
    if argv[1:] == ["--control"]:
        bad = findings(CONTROL_BAD, "control-bad")
        good = findings(CONTROL_GOOD, "control-good")
        if len(bad) == 9 and not good:
            return 0
        print(f"control misjudged: {len(bad)} of 9 bad declarations flagged, "
              f"{len(good)} false positives: {bad + good}", file=sys.stderr)
        return 1
    if argv[1:2] == ["--root"] and len(argv) == 3:
        paths = list(tree_files(argv[2]))
    elif len(argv) > 1 and not argv[1].startswith("-"):
        paths = argv[1:]
    else:
        print(__doc__, file=sys.stderr)
        return 2
    out = []
    for p in paths:
        try:
            with open(p, encoding="utf-8", errors="replace") as fh:
                out += findings(fh.read(), p)
        except OSError as e:
            out.append(f"{p}: unreadable: {e}")
    for line in out:
        print(line)
    return 1 if out else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
