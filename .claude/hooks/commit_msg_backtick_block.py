#!/usr/bin/env python3
# block-via: exit 2 (a `git commit -m "<body>"` whose DOUBLE-QUOTED body carries
# an unescaped backtick or `$(` -- the shell substitutes it before git sees it)
"""PreToolUse (Bash): stop the shell from eating a commit message.

WHY THIS IS A PRE-TOOL HOOK AND NOT A `commit-msg` HOOK. Bash performs command
substitution inside DOUBLE quotes at parse time, so by the time git runs, the
backticks are gone and their output is in the message. A `commit-msg` hook
receives the already-damaged text and finds nothing wrong with it -- it is
structurally too late. The only place the original intent still exists is the
command string, which is what this sees.

MEASURED HARM, not theoretical: a commit message describing a change wrote
`` `s*XREF:` `` into a double-quoted body, the shell expanded it, and a file
named `s*XREF:` appeared in the repository root. The same hazard was later found
live inside a test file, where it passed only because two errors cancelled.
Three separate v13 items; this is their fix.

The repo already lints Codex prompt bodies for exactly this shape
(`scripts/lint.sh` Check 12). Commit messages were the gap: nothing checked
them, and they are the one unreviewed string this project feeds to a shell
dozens of times a day.

SAFE SHAPES, deliberately not flagged:
  * single quotes -- `git commit -m 'a `literal` body'` substitutes nothing;
  * `-F <file>` / `-F -` heredocs, which is what this repo uses for long bodies;
  * an escaped backtick inside double quotes (`\\``), which bash leaves alone.
Fail-open on any parse difficulty: a commit gate that cannot read the command
must not be the thing that stops work.
"""
import json
import re
import sys

# `-m` / `--message=` with a DOUBLE-quoted body. Single-quoted bodies are safe
# by construction and are not matched at all.
_DQ_BODY = re.compile(r'(?:-m|--message=)\s*"((?:[^"\\]|\\.)*)"')
# What the shell will act on inside those quotes: an unescaped backtick, or an
# unescaped `$(`. `\\`` and `\\$(` are literals and stay out of it.
_LIVE_SUBST = re.compile(r'(?<!\\)`|(?<!\\)\$\(')


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict) or d.get("tool_name") != "Bash":
        return 0
    cmd = (d.get("tool_input") or {}).get("command") or ""
    if "git commit" not in cmd:
        return 0
    for body in _DQ_BODY.findall(cmd):
        m = _LIVE_SUBST.search(body)
        if not m:
            continue
        snippet = body[max(0, m.start() - 30):m.start() + 40]
        sys.stderr.write(
            "[commit-msg substitution BLOCK] This `git commit -m` body is in "
            "DOUBLE quotes and contains `" + m.group(0) + "`, which bash "
            "substitutes BEFORE git sees the message:\n    ..." + snippet.strip()
            + "...\n"
            "  A commit message is the one unreviewed string this repo feeds to "
            "a shell. This exact shape once wrote a file named `s*XREF:` into "
            "the repository root.\n"
            "  Fix, in order of preference:\n"
            "    git commit -F - <<'MSG' ... MSG     <- the repo's usual shape; "
            "quoted heredoc substitutes nothing\n"
            "    git commit -m 'body with `backticks`'   <- single quotes are "
            "literal\n"
            "    escape them: \\` and \\$(\n"
            "  Rewording the message to avoid backticks is the WRONG fix -- it "
            "lets the hazard shape the history.\n")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
