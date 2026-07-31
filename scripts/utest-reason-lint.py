#!/usr/bin/env python3
"""Bind the `UTEST_RSN_*` literal set to the `UTEST_REASON_REFUSAL` tree.

`UTEST_REASON_REFUSAL` is a UTEST_MAX2 tree over every refusal reason
string. It feeds `UTEST_REASON_MAX`, which every record's fixed shape
subtracts from `UTEST_RECORD_LINE_MAX` to derive `UTEST_MAX_BINARY_NAME` --
so the tree is not documentation, it is the term that decides how long an
accepted test binary's name may be.

The failure this closes: a new `#define UTEST_RSN_...` that is never added
to the tree. Nothing breaks loudly. `UTEST_REASON_MAX` simply understates
the widest reason, the derived name bound comes out too generous, and the
record formatters reach their truncation fallback on inputs the build
proved could not overflow. Only a comment guarded that relationship.

The check is a strict SET EQUALITY in both directions:

  defined-but-unlisted  a reason that can be emitted but is not measured --
                        the failure above.
  listed-but-undefined  a tree leaf naming a macro that no longer exists.
                        This one does fail the build on its own, but it is
                        reported here because a rename leaves both halves
                        broken and one message should name both.

Membership is decided on the macro body with comments REMOVED: a reason
mentioned only in a `/* ... */` beside the tree is not measured by it, and
accepting prose as membership would make the lint agree with a build that
is already wrong. Extraction failure (no tree, or an unterminated
continuation) is a hard error rather than a silent pass -- a check that
cannot find its subject has not verified anything.

Usage:  utest-reason-lint.py [--check] [FILE ...]
        (default FILE: src/kernel/test/test_usermode.c)
Exit:   0 clean, 1 violations reported, 2 usage/extraction error
"""

import argparse
import os
import re
import sys

DEFAULT_TARGET = os.path.join("src", "kernel", "test", "test_usermode.c")

TREE_MACRO = "UTEST_REASON_REFUSAL"
_DEFINE_RE = re.compile(r"^\s*#\s*define\s+(UTEST_RSN_[A-Z0-9_]+)\b")
_LEAF_RE = re.compile(r"UTEST_LIT\s*\(\s*(UTEST_RSN_[A-Z0-9_]+)\s*\)")
_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def strip_comments(text):
    """Remove C comments so a mention in prose cannot pass for membership."""
    return _LINE_COMMENT_RE.sub("", _BLOCK_COMMENT_RE.sub(" ", text))


def extract_macro_body(text, macro):
    """Return the full body of `#define <macro> ...`, continuations included.

    Raises LookupError when the macro is absent or its final line still ends
    in a backslash (an unterminated continuation means the file was cut, and
    a partial body would under-report the leaves it holds).
    """
    lines = text.split("\n")
    start = None
    pattern = re.compile(r"^\s*#\s*define\s+%s\b" % re.escape(macro))
    for index, line in enumerate(lines):
        if pattern.match(line):
            start = index
            break
    if start is None:
        raise LookupError("no `#define %s` found" % macro)
    body = []
    index = start
    while index < len(lines):
        body.append(lines[index])
        if not lines[index].rstrip().endswith("\\"):
            break
        index += 1
    else:
        raise LookupError("`#define %s` ends in an unterminated continuation"
                          % macro)
    if lines[index].rstrip().endswith("\\"):
        raise LookupError("`#define %s` ends in an unterminated continuation"
                          % macro)
    return "\n".join(body)


def check_file(path):
    """Return a list of human-readable violations for one file."""
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    stripped = strip_comments(text)

    defined = []
    for line in stripped.split("\n"):
        match = _DEFINE_RE.match(line)
        if match:
            defined.append(match.group(1))
    if not defined:
        raise LookupError("no `#define UTEST_RSN_*` reasons found in %s"
                          % path)

    body = extract_macro_body(stripped, TREE_MACRO)
    listed = set(_LEAF_RE.findall(body))

    violations = []
    for name in defined:
        if name not in listed:
            violations.append(
                "%s: %s is defined but is not a leaf of %s -- the derived "
                "reason maximum understates it, so every record's name bound "
                "is too generous" % (path, name, TREE_MACRO))
    for name in sorted(listed - set(defined)):
        violations.append(
            "%s: %s is a leaf of %s but no longer defined -- the tree names "
            "a reason that does not exist" % (path, name, TREE_MACRO))
    return violations


def main(argv):
    parser = argparse.ArgumentParser(
        prog="utest-reason-lint",
        description="Verify every UTEST_RSN_* literal is measured by the "
                    "UTEST_REASON_REFUSAL size tree.")
    # Accepted and ignored: lint.sh calls every checker the same way, and a
    # checker that rejected the shared flag would look like a real failure.
    parser.add_argument("--check", action="store_true",
                        help="report violations (the only mode; accepted for "
                             "call-site symmetry with the other linters)")
    parser.add_argument("files", nargs="*", metavar="FILE")
    args = parser.parse_args(argv)

    targets = args.files or [DEFAULT_TARGET]
    violations = []
    for path in targets:
        try:
            violations.extend(check_file(path))
        except (LookupError, OSError) as exc:
            # Fail CLOSED: an extraction that did not find its subject has
            # verified nothing, and reporting clean would be a lie.
            sys.stderr.write("utest-reason-lint: %s\n" % (exc,))
            return 2
    for line in violations:
        print(line)
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
