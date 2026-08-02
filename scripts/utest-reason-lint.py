#!/usr/bin/env python3
"""Bind every launcher reason literal to the size tree that measures it.

Two halves, because the launcher has two KINDS of reason and only one of
them is a single literal.

  REFUSALS   -- one `UTEST_RSN_*` literal each, measured by the
                `UTEST_REASON_REFUSAL` tree. Checked by set equality.
  COMPOSED   -- built at run time from several fragments (`timeout after `
                + digits + `ms`), measured by `UTEST_REASON_<NAME>`.
                Checked by ORDERED composition, see below.

Both feed `UTEST_REASON_MAX`, which every record's fixed shape subtracts
from `UTEST_RECORD_LINE_MAX` to derive `UTEST_MAX_BINARY_NAME`.


`UTEST_REASON_REFUSAL` is a UTEST_MAX2 tree over every refusal reason
string, so the tree is not documentation: it is the term that decides how
long an accepted test binary's name may be.

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

The COMPOSED half needs more than membership. Set equality between the
`UTEST_RSNC_*` fragments and the fragments a size macro names proves only
that each fragment is measured SOMEWHERE; a composing helper that appended
one fragment twice, or borrowed a fragment belonging to a different reason,
would satisfy it and still produce a string wider than
`UTEST_REASON_MAX` -- which is the same silently-wrong derivation the
refusal half exists to prevent. So each composed reason is checked as an
ORDERED sequence:

  `u_reason_<name>` is the ONLY code that composes `UTEST_REASON_<NAME>`.
  Its `u_append`/`u_append_uint` calls, in source order, must correspond
  term for term with that macro's `UTEST_LIT(<FRAG>)` / `UTEST_DIGITS_U32`
  terms, in source order.

The helper NAME is the binding, deliberately: a comment marker would be a
claim the code need not honour, and a rename would silently orphan it,
whereas a renamed helper leaves the lint unable to find its subject -- which
it reports as a hard error. A conditional append (the `-` sign in
`u_reason_exit`) still appears once in source order and is counted
unconditionally by the size term, which is the safe direction.

Additionally, no bare string literal may be appended into a reason: every
`u_append(...)` inside a `u_reason_*` helper must name a `UTEST_RSN_*` or
`UTEST_RSNC_*` macro, so a fragment cannot exist at a call site without
also existing in the measured set.

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

# The composed half. Each entry is one reason built at run time: the size
# macro that measures it and the helper that is the ONLY code allowed to
# compose it. Adding a sixth composed reason means adding it here -- and a
# helper with no entry is reported, so the table cannot silently fall behind
# the code it claims to cover.
COMPOSED = (
    ("UTEST_REASON_TIMEOUT", "u_reason_timeout"),
    ("UTEST_REASON_EXIT",    "u_reason_exit"),
    ("UTEST_REASON_LEAK",    "u_reason_leak"),
    ("UTEST_REASON_ISOLATE", "u_reason_isolate"),
    ("UTEST_REASON_INVALID", "u_reason_invalid"),
    ("UTEST_REASON_STALL",   "u_reason_stall"),
)

_FRAG_DEFINE_RE = re.compile(r"^\s*#\s*define\s+(UTEST_RSNC_[A-Z0-9_]+)\b")
# Ordered size terms: `UTEST_LIT(<FRAG>)` -> that fragment, `UTEST_DIGITS_U32`
# -> the numeric slot. Scanned with ONE alternation so source order is
# preserved between the two shapes.
_TERM_RE = re.compile(r"UTEST_LIT\s*\(\s*(UTEST_RSNC_[A-Z0-9_]+)\s*\)"
                      r"|(UTEST_DIGITS_U32)")
# Ordered append calls inside a helper body. Group 1 is the appended macro
# for `u_append`, group 2 marks a numeric append. A bare literal matches
# neither and is caught by _BARE_APPEND_RE below.
_APPEND_RE = re.compile(r"\bu_append\s*\([^;]*?,\s*"
                        r"(UTEST_RSN_[A-Z0-9_]+|UTEST_RSNC_[A-Z0-9_]+)\s*\)"
                        r"|\b(u_append_uint)\s*\(")
_BARE_APPEND_RE = re.compile(r"\bu_append\s*\([^;]*?,\s*(\"(?:[^\"\\]|\\.)*\")"
                             r"\s*\)")
# The parameter list contains nested parens (`char (*dst)[N]`), so the scan
# allows ONE level rather than stopping at the first `)`.
_PARAMS = r"(?:[^()]|\([^()]*\))*"
_HELPER_RE_TMPL = r"^static\s+void\s+%s\s*\(" + _PARAMS + r"\)\s*\n\{"

# The ordered check reads the helper body as TEXT: it does not preprocess, so
# an append reaching u_append through a MACRO is invisible to it. That hole
# was proven, not theorised -- a helper keeping its legitimate append and
# adding `SNEAK_APPEND(dst, &rp, cap)`, where the macro expands to a second
# identical append, linted CLEAN while the composed string carried the
# fragment twice and the size macro measured it once. Preprocessing to close
# it would make this lint depend on a compiler invocation and a full include
# path, to check five-line helpers.
#
# So the helpers are held to a CLOSED SHAPE instead: every call-shaped
# invocation inside one must be a name on this list, which makes an
# unrecognized call a violation BY CONSTRUCTION rather than something the
# regex must be taught to recognize. That fails closed against macros,
# wrappers, and whatever the next shape turns out to be.
_INVOCATION_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
_ALLOWED_INVOCATIONS = frozenset(("u_append", "u_append_uint", "if"))
# A LOOP would append an unbounded number of times while the ordered sequence
# still read as one term per append, so iteration and jumps are excluded from
# the shape these helpers are allowed to express.
_FORBIDDEN_KEYWORD_RE = re.compile(r"\b(while|for|do|goto|switch)\b")

# A fragment must be a PLAIN STRING-LITERAL SEQUENCE, because `UTEST_LIT` is
# `sizeof(s) - 1`: give it anything that is not an array and it measures the
# decayed type instead. `("much longer text than this" + 0)` measures
# `sizeof(char *) - 1` = 7 while appending the whole string -- a size macro
# that silently reports the wrong number, which is the exact failure the tree
# exists to prevent, reached without any suspicious syntax.
_FRAG_BODY_RE = re.compile(
    r"^\s*#\s*define\s+(UTEST_RSNC_[A-Z0-9_]+)\s+(.*)$")
_STRING_SEQ_RE = re.compile(r'^(?:"(?:[^"\\]|\\.)*"\s*)+$')
# A fragment is now spliced into printf-style FORMAT strings as well as
# appended as data (the human verdict lines concatenate it), and klog carries
# no `__attribute__((format(printf, ...)))`, so a `%` inside one would become
# an undiagnosed conversion specifier consuming a vararg that was never
# passed. Harmless while fragments were only ever u_append'ed; a real hazard
# the moment they reached a format string.
_FRAG_PERCENT_RE = re.compile(r"%")
# The append primitives must be FUNCTIONS, not macros. A `#define u_append(...)`
# expanding to two real appends leaves the helper body spelling only the
# allowed token, so every check above passes while the composed string is
# twice as wide as its bound.
_APPEND_MACRO_RE = re.compile(
    r"^\s*#\s*define\s+(u_append|u_append_uint)\b")

# Every numeric term in a size macro is `UTEST_DIGITS_U32` -- ten digits. The
# ordered check reads each `u_append_uint` as one `<digits>` token and cannot
# see the VALUE, so a helper declaring a 64-bit parameter can render twenty
# digits while the lint stays clean and the derived name bound silently
# becomes false. The parameter TYPE is what pins the domain, so it is checked:
# a composing helper takes 32-bit numerics, and widening one to match a wider
# caller trips this instead of quietly invalidating the term.
_HELPER_PARAM_RE = re.compile(r"\(([^)]*)\)")
_ALLOWED_NUMERIC_PARAMS = frozenset(("uint32_t", "int32_t"))


def strip_comments(text):
    """Remove C comments so a mention in prose cannot pass for membership.

    LITERAL-AWARE, scanned character by character rather than by regex. A
    regex sweep for `/*...*/` does not know what a string is, so a helper
    holding `const char *open = "/*";` ... `const char *close = "*/";` had
    everything between the two erased BEFORE any check ran -- a duplicated
    append in that region simply vanished and the file linted clean. That
    was confirmed against this checker, and it defeated the whole point of
    reading the helper bodies, so the scanner keeps comment delimiters that
    appear inside string and character literals.

    Comment bodies become a single space, which keeps tokens on either side
    from being fused into one identifier.
    """
    out = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if char in "\"'":
            quote = char
            start = index
            index += 1
            while index < length and text[index] != quote:
                # A backslash escapes the next character, including the quote
                # itself, so `'\''` and "\"" do not end their literal early.
                index += 2 if text[index] == "\\" else 1
            index += 1
            out.append(text[start:min(index, length)])
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            index = length if end < 0 else end + 2
            out.append(" ")
            continue
        if text.startswith("//", index):
            end = text.find("\n", index)
            index = length if end < 0 else end
            continue
        out.append(char)
        index += 1
    return "".join(out)


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


def helper_numeric_params(text, name):
    """Return the declared types of a composing helper's numeric parameters.

    Raises LookupError when the signature is absent, for the same reason the
    body extractor does: a check that cannot find its subject has verified
    nothing.
    """
    match = re.search(r"^static\s+void\s+%s\s*\((%s)\)" % (re.escape(name), _PARAMS),
                      text, re.MULTILINE)
    if match is None:
        raise LookupError("no `static void %s(...)` signature found" % name)
    types = []
    for param in match.group(1).split(","):
        param = param.strip()
        # The destination buffer is not a numeric term, in either spelling:
        # `char *dst` or the sized `char dst[UTEST_REASON_BUF]`.
        if not param or "*" in param or "[" in param:
            continue
        words = param.split()
        if len(words) >= 2:
            types.append((words[-1], " ".join(words[:-1])))
    return types


def extract_function_body(text, name):
    """Return the brace-balanced body of `static void <name>(...)`.

    Raises LookupError when the helper is absent or its braces never close.
    Absence is a HARD error rather than a skip: the helper name IS the
    binding between a composition and the size macro that measures it, so a
    rename must break the check loudly instead of quietly verifying nothing.
    """
    match = re.search(_HELPER_RE_TMPL % re.escape(name), text, re.MULTILINE)
    if match is None:
        raise LookupError("no `static void %s(...)` definition found -- the "
                          "composed reason it builds is unchecked" % name)
    depth = 0
    start = text.index("{", match.start())
    index = start
    # LITERAL-AWARE, because a counter that reads raw characters stops at the
    # `}` inside `(void)'}';` and every statement after it escapes the checks
    # below -- with the legitimate append placed BEFORE it, the truncated body
    # still matches its size macro and the lint reports clean. Confirmed
    # against this checker before the scan was made literal-aware.
    while index < len(text):
        char = text[index]
        if char in "\"'":
            quote = char
            index += 1
            while index < len(text) and text[index] != quote:
                index += 2 if text[index] == "\\" else 1
            index += 1
            continue
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
        index += 1
    raise LookupError("`%s` has an unbalanced body" % name)


def size_terms(body):
    """Ordered size terms of a composed-reason macro body."""
    return [frag if frag else "<digits>"
            for frag, digits in _TERM_RE.findall(body)]


def append_terms(body):
    """Ordered append terms of a composing helper body."""
    return [macro if macro else "<digits>"
            for macro, uint in _APPEND_RE.findall(body)]


def check_composed(path, text, fragments):
    """Ordered-composition check for every composed reason.

    Returns violations. `fragments` is the set of defined `UTEST_RSNC_*`
    names, used for the set-equality half.
    """
    violations = []
    measured = set()
    for line in text.split("\n"):
        match = _APPEND_MACRO_RE.match(line)
        if match:
            violations.append(
                "%s: %s is #defined as a macro -- the composed-reason checks "
                "read the helper bodies as written, so a macro expanding to "
                "more than one append would leave every one of them passing "
                "over a string wider than its bound"
                % (path, match.group(1)))
        match = _FRAG_BODY_RE.match(line)
        if match:
            body = match.group(2).strip()
            if not _STRING_SEQ_RE.match(body):
                violations.append(
                    "%s: %s is not a plain string literal (%s) -- UTEST_LIT "
                    "is sizeof(s)-1, so anything that decays is measured as a "
                    "pointer while the full text is still appended"
                    % (path, match.group(1), body[:40]))
            elif _FRAG_PERCENT_RE.search(body):
                violations.append(
                    "%s: %s contains a `%%` -- fragments are spliced into "
                    "printf-style format strings as well as appended as data, "
                    "and klog has no format attribute, so it would be read as "
                    "a conversion specifier consuming an absent argument"
                    % (path, match.group(1)))
    for macro, helper in COMPOSED:
        macro_body = extract_macro_body(text, macro)
        helper_body = extract_function_body(text, helper)
        want = size_terms(macro_body)
        got = append_terms(helper_body)
        measured.update(t for t in want if t != "<digits>")
        if not want:
            violations.append(
                "%s: %s names no UTEST_RSNC_* fragment or digit term -- the "
                "composition it is supposed to measure cannot be checked"
                % (path, macro))
            continue
        if want != got:
            violations.append(
                "%s: %s composes %s but %s measures %s -- the run-time string "
                "and the bound that reserves room for it are different "
                "compositions" % (path, helper, " + ".join(got) or "(nothing)",
                                  macro, " + ".join(want)))
        for literal in _BARE_APPEND_RE.findall(helper_body):
            violations.append(
                "%s: %s appends the bare literal %s -- a fragment that exists "
                "only at the call site is not measured by %s"
                % (path, helper, literal, macro))
        # Closed shape. Anything the ordered check cannot read is refused
        # rather than skipped: an invocation this lint does not understand
        # may append, and it would not appear in `got`.
        for name in sorted(set(_INVOCATION_RE.findall(helper_body))):
            if name not in _ALLOWED_INVOCATIONS:
                violations.append(
                    "%s: %s invokes %s(), which this check cannot read as an "
                    "append -- a macro or wrapper can compose a fragment "
                    "invisibly, so only %s are permitted inside a composing "
                    "helper"
                    % (path, helper, name,
                       "/".join(sorted(_ALLOWED_INVOCATIONS))))
        for pname, ptype in helper_numeric_params(text, helper):
            if ptype not in _ALLOWED_NUMERIC_PARAMS:
                violations.append(
                    "%s: %s takes `%s %s`, but %s reserves UTEST_DIGITS_U32 "
                    "for it -- a wider value renders more digits than the "
                    "bound measures, and the ordered check cannot see a value"
                    % (path, helper, ptype, pname, macro))
        forbidden = sorted(set(_FORBIDDEN_KEYWORD_RE.findall(helper_body)))
        if forbidden:
            violations.append(
                "%s: %s uses %s -- iteration or a jump can append more times "
                "than the ordered sequence shows, so %s would no longer bound "
                "what it composes"
                % (path, helper, ", ".join("`%s`" % k for k in forbidden),
                   macro))
    for name in sorted(fragments - measured):
        violations.append(
            "%s: %s is defined but no composed reason measures it -- a "
            "fragment outside every size macro is free to grow past the "
            "reservation" % (path, name))
    for name in sorted(measured - fragments):
        violations.append(
            "%s: %s is measured by a composed reason but no longer defined -- "
            "the size macro names a fragment that does not exist"
            % (path, name))
    return violations


def check_file(path):
    """Return a list of human-readable violations for one file."""
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    stripped = strip_comments(text)

    defined = []
    fragments = set()
    for line in stripped.split("\n"):
        match = _DEFINE_RE.match(line)
        if match:
            defined.append(match.group(1))
        match = _FRAG_DEFINE_RE.match(line)
        if match:
            fragments.add(match.group(1))
    if not defined:
        raise LookupError("no `#define UTEST_RSN_*` reasons found in %s"
                          % path)
    if not fragments:
        raise LookupError("no `#define UTEST_RSNC_*` fragments found in %s -- "
                          "the composed reasons are unchecked" % path)

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
    violations.extend(check_composed(path, stripped, fragments))
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
