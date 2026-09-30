#!/usr/bin/env python3
"""Lint: the site tooling parses HTML and URLs with real parsers, never regexes.

`scripts/site/*.py` reads HTML through `html.parser`, Markdown through
markdown-it, and URLs through `urllib.parse` (urlsplit, parse_qs) or Node's
WHATWG `URL`. Every hand-written approximation of those grammars that a review
found was a regex (a heading scanner, an attribute regex, three URL
serializers, a raw-anchor regex: 40 `parser-approximation` findings by
2026-09-29), and each one was caught by a Codex round because nothing
deterministic would. This check is that deterministic net.

It reads each file's AST and folds every pattern handed to the `re` module
(compile, search, match, fullmatch, sub, subn, findall, finditer, split),
through `import re as x`, `from re import compile`, and names bound to string
constants anywhere in the file, including concatenations. A folded pattern is
reported when it reads markup (`<` opening a tag-like construct: a letter,
`/`, `!`, `?`, a class `[`, a group `(`, an escape, `.` or `^`, after any
optional whitespace; or an `href`,
`src` or `srcset` attribute name) or URL structure (`://`, `http`, `mailto:`,
`www.`, a registered-domain suffix such as `\\.com`, or RFC 3986's scheme
grammar, a character class holding `+` and `.` followed by `:`).

A regex that reads the repository's OWN syntax (a `<!-- docs: -->`
directive) is not an approximation of anyone's grammar, and a few lexical
checks are deliberate; those carry a waiver on the call's line or the line
above it:

    X_RE = re.compile(r"<!-- project:...")   # parser-allow: <reason, 20+ chars>

A waiver on a line with nothing to waive is reported as stale.

`--control` runs the check over scripts/site/tests/fixtures/parser_lint/*.py,
whose lines are marked `# expect: flag` or `# expect: clean`, and fails unless
every marked line is judged as marked: a detector that goes quiet fails the
lint instead of passing it.

Usage: python3 scripts/site/check_parsers.py [--control] [FILE ...]
Exit: 0 clean, 1 findings (or a control that did not fire), 2 usage error.
"""

from __future__ import annotations

import ast
import io
import sys
import tokenize
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
FIXTURES = HERE / "tests" / "fixtures" / "parser_lint"
RE_FUNCS = frozenset({"compile", "search", "match", "fullmatch", "sub", "subn", "findall", "finditer", "split"})
WAIVER = "parser-allow:"
MIN_REASON = 20
MAX_FOLDS = 64            # distinct candidate strings per expression; more is reported as unreadable
MAX_STEPS = 20_000        # fold steps per call site; past it the pattern is reported as unreadable
MAX_DEPTH = 8             # nested names and concatenations; deeper is reported, never silently cut
MAX_PATTERN = 16_384      # characters in one folded candidate; longer is reported as unreadable
TAG_START = frozenset("/!?[(\\.^")
DOMAIN_SUFFIXES = ("com", "org", "net", "io", "co", "dev", "app", "edu", "gov")
ATTR_NAMES = ("href", "srcset", "src")


def _strip_group_syntax(p: str) -> str:
    """The pattern without the `<` that belongs to regex syntax rather than to the
    text matched: lookbehinds `(?<=`/`(?<!` and named groups `(?P<n>`/`(?<n>`."""
    out, i = [], 0
    while i < len(p):
        if p.startswith(("(?<=", "(?<!"), i):
            out.append("(?")
            i += 4
            continue
        for opener in ("(?P<", "(?<"):
            if p.startswith(opener, i):
                end = p.find(">", i + len(opener))
                name = p[i + len(opener):end] if end > 0 else ""
                if name.isidentifier():
                    out.append("(")
                    i = end + 1
                    break
        else:
            if p.startswith("\\\\", i):
                out.append(p[i:i + 2])
                i += 2
                continue
            if p.startswith("\\<", i):        # an escaped `<` is still a literal `<`
                out.append("<")
                i += 2
                continue
            out.append(p[i])
            i += 1
    return "".join(out)


def _word_at(text: str, i: int, word: str) -> bool:
    """`word` at `i`, not inside a longer identifier. A regex escape before it
    (`\\bhref`, `\\shref`) is a boundary, not part of the word."""
    if not text.startswith(word, i):
        return False
    before = text[i - 1] if i else ""
    after = text[i + len(word)] if i + len(word) < len(text) else ""
    escaped = i >= 2 and text[i - 2] == "\\"
    return (not (before.isalnum() or before == "_") or escaped) and not (after.isalnum() or after == "_")


def _scheme_class(p: str) -> bool:
    """A character class holding both `+` and `.`, quantified and followed by
    `:`: RFC 3986's scheme grammar, `[a-z0-9+.-]*:`."""
    i = p.find("[")
    while i != -1:
        j = i + 1
        while j < len(p) and p[j] != "]":
            j += 2 if p[j] == "\\" else 1
        cls = p[i + 1:j]
        k = j + 1
        if k < len(p) and p[k] in "*+?{":
            if p[k] == "{":
                k = p.find("}", k) + 1 or len(p)
            else:
                k += 1
        if "+" in cls and "." in cls and p[k:k + 1] == ":":
            return True
        i = p.find("[", j)
    return False


def _skip_space(p: str, i: int) -> int:
    """Past any regex whitespace tokens at `i`: ` `, `\\s`, each optionally
    quantified with `*`, `+` or `?`."""
    while True:
        if p.startswith(" ", i):
            i += 1
        elif p.startswith("\\s", i):
            i += 2
        else:
            return i
        if p[i:i + 1] in ("*", "+", "?"):
            i += 1


def _quoted_attribute(p: str) -> str:
    """`name="` in any regex spelling (`\\s*=\\s*`, `["']`, `\\"`): a pattern
    pulling a quoted attribute value out of markup ("" when none)."""
    i = p.find("=")
    while i != -1:
        k = i
        while k > 0 and p[k - 1] in " *+?s\\" and (p[k - 1] != "s" or p[k - 2:k - 1] == "\\"):
            k -= 1                                # back over `\s*` and spaces before the `=`
        n = k
        while n > 0 and (p[n - 1].isalnum() or p[n - 1] in "-_:"):
            n -= 1
        name = p[n:k]
        after = p[_skip_space(p, i + 1):]
        if name and name[0].isalpha() and (after[:1] in ("\"", "'") or after.startswith(("\\\"", "\\'", "[\"", "['"))):
            return f"{name}={after[:2]}"
        i = p.find("=", i + 1)
    return ""


def classify(pattern: str) -> str:
    """Why `pattern` reads HTML or URLs ("" when it does not)."""
    p = _strip_group_syntax(pattern)
    low = p.lower()
    for i, ch in enumerate(p):
        if ch != "<":
            continue
        j = i + 1
        while p[j:j + 1] == ")":                 # `(?<=<)a`: the `<` a lookbehind requires, then the tag name
            j += 1
        # Optional whitespace (`\s*`, `\s?`, ` *`) can match nothing, so `<\s*img` still reads
        # `<img`; only REQUIRED whitespace (`\s+`, a bare space) or a digit makes a comparison.
        while True:
            if p.startswith(("\\s*", "\\s?"), j):
                j += 3
            elif p.startswith((" *", " ?"), j):
                j += 2
            else:
                break
        nxt = p[j:j + 1]
        if nxt == " " or (nxt == "\\" and p[j + 1:j + 2] in ("s", "d", "")):   # `< b`, `<\s+b`, `<\d`
            continue
        if nxt and (nxt.isascii() and nxt.isalpha() or nxt in TAG_START):
            return f"markup ({p[i:i + 12]!r})"
    for word in ATTR_NAMES:
        i = low.find(word)
        while i != -1:
            if _word_at(low, i, word) and not low.startswith("/", i + len(word)):
                return f"an HTML attribute ({word})"
            i = low.find(word, i + 1)
    quoted = _quoted_attribute(p)
    if quoted:
        return f"an HTML attribute ({quoted})"
    for marker in ("://", ":\\/\\/", ":/{2}", "mailto:", "www\\."):
        if marker in low:
            return f"URL structure ({marker})"
    i = low.find("http")
    while i != -1:
        if _word_at(low, i, "http") or _word_at(low, i, "https") or low.startswith("https?", i):
            return "URL structure (http)"
        i = low.find("http", i + 1)
    for suffix in DOMAIN_SUFFIXES:
        needle = "\\." + suffix
        i = low.find(needle)
        while i != -1:
            after = low[i + len(needle):i + len(needle) + 1]
            if not (after.isalnum() or after == "_"):
                return f"a host name ({needle})"
            i = low.find(needle, i + 1)
    if _scheme_class(p):
        return "URL structure (scheme grammar)"
    return ""


class _TooComplex(Exception):
    """A pattern expression past MAX_FOLDS candidates, MAX_STEPS steps or MAX_DEPTH levels (internal)."""


class _Scan(ast.NodeVisitor):
    def __init__(self, tree: ast.AST) -> None:
        self.modules: set[str] = set()          # names bound to the re module
        self.funcs: set[str] = set()            # names bound to a re pattern function
        self.consts: dict[str, list[ast.expr]] = {}
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                for a in node.names:
                    if a.name == "re":
                        self.modules.add(a.asname or "re")
            elif isinstance(node, ast.ImportFrom) and node.module == "re":
                for a in node.names:
                    if a.name in RE_FUNCS:
                        self.funcs.add(a.asname or a.name)
            elif isinstance(node, (ast.Assign, ast.AnnAssign)) and node.value is not None:
                targets = node.targets if isinstance(node, ast.Assign) else [node.target]
                for t in targets:
                    if isinstance(t, ast.Name):
                        self.consts.setdefault(t.id, []).append(node.value)
        self.calls: list[tuple[ast.Call, list[str] | None]] = []
        self._memo: dict[str, list[str]] = {}
        self._active: set[str] = set()
        self._cuts = 0
        self._steps = 0

    def bounded(self, candidates) -> list[str]:
        """The distinct candidates, in order. Every candidate enumerated, repeat
        or not, is charged to the call's step budget, and more than MAX_FOLDS
        distinct ones is a pattern this check cannot fully read: both are
        reported rather than judged on a sample (the forbidden candidate may be
        the 65th)."""
        out: dict[str, None] = {}
        for c in candidates:
            self._steps += 1
            if self._steps > MAX_STEPS:
                raise _TooComplex()
            out[c] = None
            if len(out) > MAX_FOLDS:
                raise _TooComplex()
        return list(out) or [""]

    def concat(self, parts: list[list[str]]) -> list[str]:
        """Every concatenation of one candidate per part, deduplicated after each
        part, so no step enumerates more than MAX_FOLDS x MAX_FOLDS strings however
        many parts repeat an ambiguous name (`f"{A}{A}...{A}"`). A candidate longer
        than MAX_PATTERN is refused BEFORE it is built: names that double at each
        step would otherwise allocate without bound while every other limit holds."""
        acc = [""]
        for part in parts:
            longest = max(map(len, acc)) + max(map(len, part))
            if longest > MAX_PATTERN:
                raise _TooComplex()
            acc = self.bounded(a + b for a in acc for b in part)
        return acc

    def fold(self, node: ast.expr | None, depth: int = 0) -> list[str]:
        """Every string `node` can denote, as far as the file's constants say;
        a part it cannot know folds to the empty string."""
        self._steps += 1
        if self._steps > MAX_STEPS or depth > MAX_DEPTH:
            raise _TooComplex()
        if node is None:
            return [""]
        if isinstance(node, ast.Constant) and isinstance(node.value, (str, bytes)):
            v = node.value
            return [v.decode("latin-1") if isinstance(v, bytes) else v]
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
            return self.concat([self.fold(node.left, depth + 1), self.fold(node.right, depth + 1)])
        if isinstance(node, ast.JoinedStr):   # an f-string: each {expr} folds like any other part
            return self.concat([self.fold(v.value, depth + 1) if isinstance(v, ast.FormattedValue)
                                else [v.value if isinstance(v, ast.Constant) else ""] for v in node.values])
        if isinstance(node, ast.Name) and node.id in self.consts:
            # Each name folds once (memoized) and a name met again while it is being
            # folded (`P = P`, `A = B; B = A`) contributes nothing, so the work is
            # linear in the file however the bindings chain.
            # A fold cut short by a cycle is not cached, so which call site is folded
            # first can never hide another's pattern; MAX_STEPS and MAX_DEPTH bound the
            # uncached work instead, and a pattern past either is reported, not passed.
            if node.id in self._memo:
                return self._memo[node.id]
            if node.id in self._active:
                self._cuts += 1
                return [""]
            cuts = self._cuts
            self._active.add(node.id)
            try:
                out = self.bounded(x for value in self.consts[node.id] for x in self.fold(value, depth + 1))
            finally:
                self._active.discard(node.id)
            if self._cuts == cuts:
                self._memo[node.id] = out
            return out
        return [""]

    def visit_Call(self, node: ast.Call) -> None:
        f = node.func
        is_re = (isinstance(f, ast.Attribute) and f.attr in RE_FUNCS and isinstance(f.value, ast.Name)
                 and f.value.id in self.modules) or (isinstance(f, ast.Name) and f.id in self.funcs)
        if is_re:
            arg = node.args[0] if node.args else next((k.value for k in node.keywords if k.arg == "pattern"), None)
            self._steps = 0
            try:
                self.calls.append((node, self.fold(arg)))
            except _TooComplex:
                self._active.clear()
                self.calls.append((node, None))
        self.generic_visit(node)


def _waivers(text: str) -> dict[int, tuple[str, bool]]:
    """{line: (reason, stands alone on its line)} of every `# parser-allow:`
    COMMENT, read by the tokenizer, so the words inside a string or a docstring
    are never taken for one."""
    out: dict[int, tuple[str, bool]] = {}
    for tok in tokenize.generate_tokens(io.StringIO(text).readline):
        if tok.type == tokenize.COMMENT:
            body = tok.string[1:].strip()
            if body.startswith(WAIVER):   # the reason ends where a second comment starts
                out[tok.start[0]] = (body[len(WAIVER):].split(" #", 1)[0].strip(), tok.line.lstrip().startswith("#"))
    return out


def _comments(text: str) -> dict[int, str]:
    return {t.start[0]: t.string for t in tokenize.generate_tokens(io.StringIO(text).readline)
            if t.type == tokenize.COMMENT}


def check_file(path: Path) -> list[tuple[int, str]]:
    """[(line, message)] for one file."""
    text = path.read_text(encoding="utf-8")
    try:
        tree = ast.parse(text, filename=str(path))
    except SyntaxError as e:
        return [(e.lineno or 1, f"cannot parse: {e.msg}")]
    scan = _Scan(tree)
    scan.visit(tree)
    waivers = _waivers(text)
    used: set[int] = set()
    found: list[tuple[int, str]] = []
    for call, patterns in scan.calls:
        why = "?" if patterns is None else next((w for w in (classify(p) for p in patterns) if w), "")
        if not why:
            continue
        # A waiver trails one of the call's own lines, or stands alone on the line above it.
        own = [n for n in range(call.lineno, (call.end_lineno or call.lineno) + 1) if n in waivers]
        above = call.lineno - 1
        at = own[0] if own else above if above in waivers and waivers[above][1] else None
        if at is not None:
            used.add(at)
            if len(waivers[at][0]) < MIN_REASON:
                found.append((at, f"parser-allow waiver needs a reason of {MIN_REASON}+ characters"))
            continue
        if patterns is None:
            found.append((call.lineno, f"regex pattern too complex to fold ({MAX_FOLDS} candidates, {MAX_STEPS} steps, {MAX_DEPTH} levels, "
                                       f"{MAX_PATTERN} characters); build it from "
                                       f"plain constants, or waive it with `# {WAIVER} <reason>`"))
            continue
        shown = next(p for p in patterns if classify(p))
        found.append((call.lineno, f"regex reads {why}: {shown[:70]!r}; parse it with html.parser, markdown-it "
                                   f"or urllib.parse, or waive it with `# {WAIVER} <reason>`"))
    for n in sorted(set(waivers) - used):
        found.append((n, "stale parser-allow waiver: no HTML or URL regex on its line (or, for a comment on "
                          "its own line, the next)"))
    return sorted(found)


def default_files() -> list[Path]:
    return sorted(HERE.glob("*.py"))


def run(files: list[Path]) -> int:
    bad = 0
    for path in files:
        for line, msg in check_file(path):
            rel = path.resolve().relative_to(REPO) if path.resolve().is_relative_to(REPO) else path
            print(f"{rel}:{line}: {msg}")
            bad += 1
    return 1 if bad else 0


def control() -> int:
    """Every `# expect: flag` line reported, no `# expect: clean` line reported."""
    files = sorted(FIXTURES.glob("*.py"))
    if not files:
        print(f"control: no fixtures under {FIXTURES.relative_to(REPO)}")
        return 1
    wrong, marked = [], 0
    for path in files:
        comments = _comments(path.read_text(encoding="utf-8"))
        flagged = {n for n, _ in check_file(path)}
        for n, raw in sorted(comments.items()):
            want = "flag" if "# expect: flag" in raw else "clean" if "# expect: clean" in raw else None
            if want is None:
                continue
            marked += 1
            if (n in flagged) != (want == "flag"):
                wrong.append(f"{path.relative_to(REPO)}:{n}: expected {want}, got {'flag' if n in flagged else 'clean'}")
    for w in wrong:
        print(f"control: {w}")
    if not marked:
        print("control: the fixtures mark no lines")
        return 1
    print(f"control: {marked - len(wrong)}/{marked} fixture lines judged as marked")
    return 1 if wrong else 0


def main(argv: list[str]) -> int:
    if argv[:1] == ["--control"]:
        return control() if len(argv) == 1 else 2
    if any(a.startswith("-") for a in argv):
        print(__doc__.strip().splitlines()[-2], file=sys.stderr)
        return 2
    return run([Path(a) for a in argv] or default_files())


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
