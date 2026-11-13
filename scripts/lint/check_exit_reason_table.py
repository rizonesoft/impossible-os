#!/usr/bin/env python3
"""Lint Check 39: a reserved exit reason is declared only in TASK_EXIT_REASONS.

include/kernel/sched/task.h generates every reserved exit reason, its count,
_LAST and the classifier bounds from one X-macro table. A reason declared
anywhere else (a `#define TASK_EXIT_X (TASK_EXIT_REASON_LAST - 1)`, an enum
or const `TASK_EXIT_X = ...`) builds clean and classifies as an ordinary
application status, which is the defect the table exists to make impossible.

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
# An enumerator or initialised constant: NAME = value (not ==).
ASSIGN = re.compile(r"\b(TASK_EXIT_[A-Z][A-Z0-9_]*)[ \t]*=(?!=)")
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def findings(text, path):
    # Comments are blanked to the same line count, so line numbers survive.
    body = COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    out = []
    for rx, shape in ((DEFINE, "#define"), (ASSIGN, "declaration")):
        for m in rx.finditer(body):
            if m.group(1) in ALLOWED:
                continue
            line = body.count("\n", 0, m.start()) + 1
            out.append(f"{path}:{line}: {shape} of {m.group(1)} outside TASK_EXIT_REASONS "
                       f"(add it as a table row instead)")
    return out


CONTROL_BAD = """\
/* #define TASK_EXIT_IN_A_COMMENT (TASK_EXIT_REASON_LAST - 1) */
#define TASK_EXIT_UTEST_PROBE (TASK_EXIT_REASON_LAST - 1)
enum { TASK_EXIT_UTEST_PROBE2 = TASK_EXIT_REASON_BASE - 9 };
static const int TASK_EXIT_UTEST_PROBE3 = -1007;
"""
CONTROL_GOOD = """\
#define TASK_EXIT_REASON_BASE (-1000)
#define TASK_EXIT_REASON_CONST(name, ord) TASK_EXIT_##name = TASK_EXIT_REASON_BASE - (ord),
#define TASK_EXIT_REASON_LAST (TASK_EXIT_REASON_BASE - TASK_EXIT_REASON_COUNT)
if (status == TASK_EXIT_UTEST_TIMEOUT) t->exit_status = TASK_EXIT_UTEST_TIMEOUT;
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
        if len(bad) == 3 and not good:
            return 0
        print(f"control misjudged: {len(bad)} of 3 bad declarations flagged, "
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
