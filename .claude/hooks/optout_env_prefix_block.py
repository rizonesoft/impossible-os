#!/usr/bin/env python3
# block-via: exit 2 (an opt-out env prefix on a NON-FIRST command in a chain --
# the gate it is meant to satisfy scans only the LEADING prefix and never sees it)
"""PreToolUse (Bash): catch an opt-out that will be silently dropped.

THE LOSING SHAPE, in one line:

    git add path && SKIP_REVIEW_HOOK=1 git commit -m "..."

That is valid bash and the variable genuinely reaches `git commit` -- which is
exactly why it keeps getting written. The problem is that the reader is not
bash. Every PreToolUse gate in this repo resolves its opt-out through
`.claude/hooks/_skip_env.py`, and `_scan_inline` walks the LEADING env prefix
of the command string and stops at the first non-wrapper token
(`_skip_env.py:_scan_inline`, and the same rule in `_scan_prefix_no_shlex`).
In the command above the first real token is `git`, so the walk stops before
the assignment and returns `{}`. The gate then blocks -- while its own message
tells the caller to set the variable they just set.

WHY A HOOK AND NOT MORE DOCUMENTATION. This is in CLAUDE.md, and it is in the
operator's long-term memory as a named quirk, and it STILL recurred on
2026-08-10. Knowledge that is not present at the moment of use is not reaching
the moment of use. A gate is; that is the whole argument.

WHY NOT JUST MAKE `_skip_env` SCAN THE WHOLE CHAIN. Because that would be a
real widening of every opt-out in the repo: `_scan_inline`'s stop-at-first-token
rule is what makes it impossible to arm an opt-out from inside arbitrary data
(`echo SKIP_REVIEW_HOOK=1`, a commit message quoting the variable, a heredoc
body). Trading that property away to save a `&&` would buy convenience with the
one guarantee the scanner has. Fixing the CALLER is the cheaper side.

FAIL-OPEN on any parse difficulty, and deliberately narrow: it fires only on a
KNOWN opt-out key, only when that key sits after a chain operator, and only
when the same key is not ALSO present as a leading prefix (which would make the
call work regardless). A gate that guesses would be worse than the quirk.
"""
import json
import re
import sys

# The opt-out keys this repo's PreToolUse gates actually read, per their
# `read_skip_envs(..., keys=(...))` call sites. Listed EXPLICITLY rather than
# matched as `SKIP_*` for the same reason _skip_env takes an explicit key list:
# a prefix match would sweep up a secret that merely shares the prefix.
_OPTOUT_KEYS = (
    "SKIP_REVIEW_HOOK",
    "SKIP_SKILL_STEP_BLOCK",
    "SKIP_PHASE1_BLOCK",
    "SKIP_REVIEW_PIPELINE",
    "SKIP_RUNNER_SUITE",
    "SKIP_HOOK_AUDIT",
    "SKIP_CI_PARITY",
    "SKIP_TOOLING_SUITE",
    "SKIP_IDENTITY_GATE",
    "SKIP_LINT_PROMPT_ESCAPING",
    "CODEX_FLAG_OVERRIDE",
    "READ_CACHE_DISABLE",
    "ATTENDED_REPAIR_OVERRIDE",
)

# Wrapper tokens `_skip_env` walks PAST before the first real token. Mirrored
# here so this hook's idea of "leading prefix" matches the scanner's exactly --
# two definitions that can disagree would make this gate fire on calls that
# actually work.
_WRAPPERS = r"(?:env|sudo|doas|nice|nohup|timeout|ionice|stdbuf|command|exec)"

# A chain operator that ENDS the leading prefix: everything after it is a new
# command, and _skip_env never looks there. `|&` and `&` (background) are
# included -- a backgrounded command is still a separate command.
_CHAIN = re.compile(r"(\|\||&&|\||;|\n|(?<![>&])&(?!&))")


def _leading_prefix_keys(segment: str) -> set:
    """The opt-out keys _skip_env would actually FIND in this segment.

    Reproduces the scanner's rule rather than approximating it: KEY=VALUE
    assignments and wrapper words, stopping at the first other token.
    """
    found = set()
    pos = 0
    tok_re = re.compile(
        r"\s*([A-Za-z_][A-Za-z0-9_]*=\S*|" + _WRAPPERS + r"\b|\S+)")
    while True:
        m = tok_re.match(segment, pos)
        if not m:
            break
        tok, pos = m.group(1), m.end()
        if "=" in tok and not tok.startswith("="):
            head = tok.split("=", 1)[0]
            if head.isidentifier():
                found.add(head)
                continue
            break
        if re.fullmatch(_WRAPPERS, tok):
            continue
        break  # first real token ends the prefix, exactly as _skip_env does
    return found


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict) or d.get("tool_name") != "Bash":
        return 0
    cmd = (d.get("tool_input") or {}).get("command") or ""
    if not isinstance(cmd, str) or not cmd:
        return 0
    # Cheap pre-filter: no known opt-out mentioned at all, nothing to say.
    if not any(k in cmd for k in _OPTOUT_KEYS):
        return 0

    segments = [s for s in _CHAIN.split(cmd) if s and not _CHAIN.fullmatch(s)]
    if len(segments) < 2:
        return 0

    # Keys that WILL be seen: the ones leading the whole command.
    seen = _leading_prefix_keys(segments[0])
    # The override obeys the very rule this hook teaches: it counts only as a
    # LEADING prefix. Honouring it from anywhere would make the hook the one
    # thing in the repo that reads an opt-out the scanners cannot.
    if "OPTOUT_PREFIX_OVERRIDE" in seen:
        return 0
    # Keys that will NOT be seen: leading a LATER segment.
    lost = set()
    for seg in segments[1:]:
        lost |= _leading_prefix_keys(seg)
    lost &= set(_OPTOUT_KEYS)
    # A key ALSO present up front works regardless -- never flag that.
    lost -= seen
    if not lost:
        return 0

    names = ", ".join(sorted(lost))
    sys.stderr.write(
        "[opt-out env-prefix BLOCK] " + names + " is set on a command that is "
        "NOT first in this chain, so the gate it is meant to satisfy will "
        "never see it.\n"
        "  Bash does set it for that command -- but the reader is not bash. "
        "PreToolUse gates resolve opt-outs through .claude/hooks/_skip_env.py, "
        "whose scan walks the LEADING env prefix and stops at the first real "
        "token (here: the first word of segment 1). The assignment is past that "
        "stop, so the scan returns nothing and the gate blocks while telling you "
        "to set the variable you just set.\n"
        "  Fix -- prefix the WHOLE call, or split it into two:\n"
        "    " + names.split(', ')[0] + "=1 <the one command that needs it>\n"
        "    (run the earlier command as its own separate Bash call)\n"
        "  This is a known-recurring shape: it is in CLAUDE.md, it is in the "
        "operator's memory as a named quirk, and it still recurred on "
        "2026-08-10. That is why it is a gate and not a note.\n"
        "  Override: OPTOUT_PREFIX_OVERRIDE=1 as the FIRST thing on the line.\n")
    return 2


if __name__ == "__main__":
    sys.exit(main())
