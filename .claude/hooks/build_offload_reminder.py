#!/usr/bin/env python3
# block-via: exit 2 (P3.4: overnight SECTIONS phase only -- BLOCK-with-reroute a
# BARE build/test/smoke/lint script into the deterministic run-artifact.sh wrapper;
# the outer-run-artifact.sh-wrapped form is exempt (prereq 1) so the sanctioned
# path never deadlocks. Interactive sessions are never gated.)
"""PreToolUse (Bash): checks-runner offload reminder for overnight SECTIONS.

When the overnight sequencer is in SECTIONS phase and the main loop runs a
bare verification script (build.sh / test.sh / test-smoke.sh / lint.sh /
test-tooling.sh) via Bash with no fresh checks-runner dispatch, inject a
systemMessage reminding it of the checks-runner route. Measured
(run-20260702-141810.log): 91 in-context build/test invocations, 0
checks-runner dispatches across 29 pipeline passes -- the skill text alone
does not hold overnight.

Freshness MUST be checks-runner-specific (fixed 2026-07-05): the original
check treated ANY recent Agent dispatch as cover, so a kernel-explorer or
parity-research-analyst call minutes earlier silenced the reminder while the
actual build/test still ran inline. Measured regression from that bug
(run-20260704-213648.log): 99 in-context build.sh/test.sh calls -- WORSE than
the 91-call baseline that motivated this hook in the first place.

P3.4 (2026-07-14): promoted from WARN to BLOCK-with-reroute. The old
"warning-only" rationale (the checks-runner subagent's own Bash traversed
this hook) is obsolete -- checks-runner is deprecated for green mechanics and
the OUTER run-artifact.sh wrapper is now exempt (prereq 1), so blocking a BARE
build/test can never deadlock the sanctioned wrapped path. A bare script in the
SECTIONS phase now returns exit 2 with the reroute message; the model re-issues
via `run-artifact.sh`. Interactive sessions are exempt (single builds are normal
there; inline_churn_monitor covers sustained interactive fan-out). Fail-open on
any error -- a hook error must never wedge the run.
"""
from __future__ import annotations

import json
import os
import re
import shlex
import sys
import time
from pathlib import Path

FRESH_NS = 900 * 1_000_000_000  # 15 min: a dispatch this recent counts

# J2b: `lint.sh` is intentionally NOT here -- lint is cheap and does not flood
# the context like a full build/test/smoke run, so BLOCKing it to force the
# run-artifact.sh reroute was mild over-reach. Only the output-heavy scripts route.
#
# R2 (2026-07-19): bypass-shape hardening. The original pattern matched ONLY
# the literal `bash scripts/X.sh` form; absolute paths, `./`-prefix, direct
# execution at a command position, `make test-*`, interpreter flags
# (`bash -x`), quoted paths, env-var prefixes (`FOO=1 scripts/test.sh`),
# subshells, and newline-separated statements all sailed through raw
# (review 2026-07-19, finders A/B/altitude -- each shape verified live).
# Three separate patterns so each detector stays auditable:
#   - interpreter form: bash/sh (+ optional flags/quote) directly before the
#     script path, anywhere in the command (so `cat scripts/test.sh` stays
#     un-gated -- no interpreter);
#   - direct-exec form: the script path at a COMMAND position (start of
#     string/line or after ; & | ( ), optionally behind env-var assignments,
#     so a quoted mention mid-string does not trip the block;
#   - make form: a `test`/`test-<suite>` target at a command position
#     ((?!=) keeps `make test=1` variable assignments out).
_SUITE_SCRIPTS = r"scripts/(?:build|test|test-smoke|test-tooling)\.sh\b"
_CMD_POS = r"(?:^|[;&|(]\s*)"
_ENV_PREFIX = r"(?:[A-Za-z_][A-Za-z0-9_]*=\S*\s+)*"
_SCRIPT_RES = (
    # LOOKBEHIND, not \b (2026-07-28). `\b` is satisfied by the DOT inside any
    # `*.sh` filename -- `.` is a non-word char, `s` is a word char -- so
    # `scripts/build.sh scripts/test.sh` contained the substring
    # `sh scripts/test.sh` and a READ-ONLY `grep`/`ls` naming two suite scripts
    # was BLOCKED as if it were invoking one. Hit live by
    # `grep -n "..." scripts/build.sh scripts/test.sh`, where the reroute
    # advice ("re-issue through run-artifact.sh") is meaningless for a grep, so
    # the only exits were rephrasing or giving up.
    # `(?<![.\w])` keeps every real interpreter form matching -- `bash x`,
    # `sh x`, `/bin/bash x`, `time bash x` -- while refusing to treat a
    # filename's tail as the shell. A plain _CMD_POS anchor would be WRONG
    # here: it would stop matching `/bin/bash ...` and `time bash ...`.
    re.compile(r"(?<![.\w])(?:bash|sh)\s+(?:-\S+\s+)*[\"']?(?:\S*/)?" + _SUITE_SCRIPTS),
    re.compile(_CMD_POS + _ENV_PREFIX + r"[\"']?(?:\./|\S*/)?" + _SUITE_SCRIPTS,
               re.MULTILINE),
    re.compile(_CMD_POS + _ENV_PREFIX
               + r"make\s+(?:\S+\s+)*?test(?:-[A-Za-z0-9_-]+)?\b(?!=)",
               re.MULTILINE),
)


# The sanctioned wrapper, recognised as an INVOCATION rather than a mention.
# A bare `\brun-artifact\.sh\b` substring test exempted any segment that merely
# contained the name -- `NOTE=run-artifact.sh bash scripts/test.sh` ran the
# suite and was excused by its own comment-like assignment. The wrapper is only
# a wrapper when it is the command being run, so require the same
# interpreter-or-command-position shapes the suite detectors use, plus the
# `-- ` that separates the wrapper's own arguments from the wrapped command.
_RUN_ARTIFACT = r"scripts/overnight/run-artifact\.sh\b"
#
# ANCHORED to the start of the segment, because the exemption belongs to the
# segment's EXECUTABLE, not to any word in it. Unanchored, a shape like
# `bash -c '<the suite>' bash .../run-artifact.sh lbl -- true` runs the suite
# out of the `-c` body while the trailing words -- mere positional arguments --
# claimed the exemption for the whole segment. Interpreter flags are allowed
# before the script, but `-c` is NOT: with `-c` the next word is a program, not
# this wrapper.
#
# The anchor also has to tolerate the shell KEYWORDS a segment can begin with.
# The splitter cuts on `;`/`&`/`|`, so a wrapped route inside a loop arrives as
# `do bash .../run-artifact.sh lbl -- bash scripts/test-tooling.sh` and the
# `^\s*` anchor missed it -- the correctly-wrapped call was BLOCKED for sitting
# in a `for` loop (v08, live 2026-08-03). `_tokens_invocation` already steps
# over the same keyword set; the exemption regex simply had not been taught it.
# NOTE the filed diagnosis blamed the loop's VARIABLE label ("stab$i"); it is
# the `do` keyword. A variable label outside a loop was never blocked.
#
# ...and the GROUPING OPENERS `(` and `{` for the same reason (v12, live
# 2026-08-08, filed three times across three sections). `command_segments`
# deliberately does NOT treat parens as separators -- a subshell's text stays
# attached so a caller sees `(bash foo.sh` with the paren intact -- so a
# correctly-wrapped call inside one arrives as `( bash .../run-artifact.sh lbl
# -- bash scripts/test-tooling.sh > /tmp/x 2>&1` and the anchor missed it. The
# inner matcher DOES split on parens, so it found the wrapped suite and the
# segment blocked: the exemption was being judged at a coarser granularity than
# the match. The shape matters because it is the rc-capture idiom the
# ship-sequence doctrine itself teaches (`( wrapper ...; echo "rc=$?" ) &`), so
# the gate was blocking the route the rest of the system prescribes.
#
# This cannot launder a bare run: the opener is STEPPED OVER, not ignored, so
# the wrapper must still be the first command of the group. `(bash
# scripts/test.sh; bash .../run-artifact.sh l -- true)` splits on the `;` and
# its first segment still blocks -- pinned below.
_RUN_ARTIFACT_RE = re.compile(
    r"^\s*(?:(?:do|then|else|elif|time|exec|nohup|command|stdbuf)\s+"
    + r"|\(\s*|\{\s+)*"
    + _ENV_PREFIX
    + "(?:"
    + r"(?:bash|sh)\s+(?:-(?!c\b)\S+\s+)*[\"']?(?:\S*/)?" + _RUN_ARTIFACT
    + "|"
    + r"[\"']?(?:\./|\S*/)?" + _RUN_ARTIFACT
    + ")"
    + r".*?\s--\s",
    re.DOTALL)


def _normalise_continuations(text: str) -> str:
    """Fold backslash-newline continuations into single spaces.

    A wrapped route split across lines for readability is the SAME invocation;
    leaving the continuation in place made the `--` land on another line and
    the anchored match fail, blocking the very route this gate recommends.
    """
    return re.sub(r"\\\n\s*", " ", text)


class _Hit:
    """Minimal re.Match stand-in: callers only ever ask for .group(0)."""

    __slots__ = ("_s",)

    def __init__(self, s):
        self._s = s

    def group(self, n=0):
        return self._s


# Wrappers AND shell keywords that stand before the real command. The keywords
# matter because the splitter's segments start after `;`/`&`/`|`, so a loop or
# conditional body arrives as `do bash scripts/test.sh` -- without `do` here the
# suite escaped through any `for`/`while` loop (review 2026-07-30; the old
# textual matcher caught it by matching anywhere).
#
# `{` is here for a BYPASS found while writing the mutation control for the
# grouping-opener fix (2026-08-08, close-out): `_split_unquoted` cuts on
# `;&|()` but NOT on braces, so `{ bash scripts/test-tooling.sh; }` tokenised to
# `['{', 'bash', 'scripts/test-tooling.sh']`, the head read as `{`, and the
# segment matched nothing at all. A brace group hid a bare suite run from the
# gate completely -- in BOTH directions, before and after that fix, which is why
# the control that was supposed to prove the fix safe is what exposed it.
_TRANSPARENT = {"time", "exec", "nohup", "command", "stdbuf",
                "do", "then", "else", "elif", "if", "while", "until", "!", "{"}
_ENV_ASSIGN_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")
# A non-executing interpreter option: `-n`/`--noexec`, including inside a short
# cluster (`-nx`). `--` alone ends option parsing and is not one of these.
_NOEXEC_RE = re.compile(r"^(?:--noexec$|-(?!-)[A-Za-z]*n[A-Za-z]*$)")
_SUITE_TOKEN_RE = re.compile(
    r"(?:^|/)scripts/(?:build|test|test-smoke|test-tooling)\.sh$")
_MAKE_TARGET_RE = re.compile(r"^test(?:-[A-Za-z0-9_-]+)?$")


def _substitution_bodies(cmd: str):
    """Bodies of `$(...)`, `` `...` ``, `<(...)` and `>(...)`, innermost included.

    Balanced-paren scan rather than a regex, because a nested backtick whose body
    contains `)` desynchronises any non-counting walk -- the exact shape the
    tooling suite pins (`"$(echo \\`echo )\\`; bash scripts/test.sh)"`).

    QUOTE- AND ESCAPE-AWARE (v08 finding, 2026-08-03). The scan used to read the
    raw string, so any backtick ANYWHERE read as a substitution -- including the
    markdown code-quotes in a commit message. Live cost: `git commit -m "...a
    plain \\`bash scripts/test.sh\\`..." -- <paths>` was BLOCKED while describing
    the gate the section had just fixed, and the answer was to reword the commit
    record to launder the vocabulary. That is the gate degrading the history it
    cannot read.

    The fix is not an exemption, it is accuracy about what bash EXECUTES:
    inside single quotes nothing substitutes, and a backslash-escaped backtick
    or `$(` does not substitute inside double quotes either. A substitution that
    really runs is still followed -- see the `live backtick still runs` test.
    """
    out = []
    n = len(cmd)
    i = 0
    quote = None
    while i < n:
        ch = cmd[i]
        # Single quotes suppress EVERY expansion, so their span cannot start a
        # substitution. Skipping the whole span (rather than tracking a flag)
        # also keeps an apostrophe inside a double-quoted body from opening one.
        if quote == "'":
            if ch == "'":
                quote = None
            i += 1
            continue
        if ch == "\\" and quote != "'":
            i += 2                      # escaped char is literal, incl. ` and $
            continue
        if ch == "'" and quote is None:
            quote = "'"
            i += 1
            continue
        if ch == '"':
            quote = None if quote == '"' else '"'
            i += 1
            continue
        if ch == "`":
            j = cmd.find("`", i + 1)
            if j < 0:
                break
            out.append(cmd[i + 1:j])
            i = j + 1
            continue
        opens = (cmd.startswith("$(", i) or cmd.startswith("<(", i)
                 or cmd.startswith(">(", i))
        if opens:
            depth, j, tick = 1, i + 2, False
            while j < n and depth:
                c = cmd[j]
                if c == "`":
                    tick = not tick
                elif not tick and c == "(":
                    depth += 1
                elif not tick and c == ")":
                    depth -= 1
                j += 1
            if depth == 0:
                body = cmd[i + 2:j - 1]
                out.append(body)
                out.extend(_substitution_bodies(body))
                i = j
                continue
            break                 # unbalanced -> leave it to the fail-closed path
        i += 1
    return out


def _split_unquoted(cmd: str):
    """Split on control operators that are OUTSIDE quotes. None if unbalanced.

    A quote-blind split was the whole defect: `_CMD_POS` treated `|` and `(` as
    command positions with no notion of quoting, so `|` and `(` appearing inside
    a quoted ARGUMENT read as the start of a new command."""
    segs, cur, quote, esc = [], [], None, False
    for ch in cmd:
        if esc:
            cur.append(ch)
            esc = False
            continue
        if ch == "\\" and quote != "'":
            cur.append(ch)
            esc = True
            continue
        if quote:
            cur.append(ch)
            if ch == quote:
                quote = None
            continue
        if ch in "'\"":
            quote = ch
            cur.append(ch)
            continue
        if ch in ";&|()\n":
            segs.append("".join(cur))
            cur = []
            continue
        cur.append(ch)
    if quote or esc:
        return None              # unterminated -> unparseable -> never block
    segs.append("".join(cur))
    return [s for s in segs if s.strip()]


def _segment_invocation(seg: str):
    """The suite invocation this ONE segment runs, or None.

    Structural, not textual: the segment is tokenised and the COMMAND is
    identified positionally, so a suite path appearing as DATA inside an
    argument is never mistaken for an invocation."""
    try:
        toks = shlex.split(seg)
    except ValueError:
        return None              # same fail-safe as _skip_env (326bc786)
    return _tokens_invocation(toks)


def _tokens_invocation(toks):
    """Same check over an already-tokenised command, so the WRAPPED route can
    recurse: `run-artifact.sh <label> -- bash scripts/test.sh` must still report
    the inner suite, because the caller logs that as a compliance `follow`
    event. Without the recursion the sanctioned route would look like no suite
    ran at all and offload-report's reroute-compliance count would read zero."""
    i = 0
    while i < len(toks) and (_ENV_ASSIGN_RE.match(toks[i])
                             or toks[i] in _TRANSPARENT):
        i += 1
    if i >= len(toks):
        return None
    head = toks[i]
    base = head.rsplit("/", 1)[-1]
    if base in ("bash", "sh"):
        j = i + 1
        while j < len(toks) and toks[j].startswith("-"):
            # `-n`/`--noexec` READS AND PARSES ONLY -- it cannot run the suite,
            # by definition. Filed three separate times (v08), because a syntax
            # check is the normal inner loop for a 7,000-line shell harness and
            # the gate refused it after every edit. The sanctioned reroute
            # (`run-artifact.sh syntax-check -- bash -n ...`) works but pays the
            # artifact wrapper for a check that produces no output to flood, and
            # the workaround actually reached for was to hide the path behind a
            # shell variable -- an evasion that works for the wrong reason.
            #
            # The exemption is on the FLAG, not on the script name, so it cannot
            # smuggle a real run past the gate: drop `-n` and the same command
            # blocks again.
            if _NOEXEC_RE.match(toks[j]):
                return None
            if toks[j] == "-c":
                # The `-c` body is a COMMAND STRING, so parse it as one. Treating
                # it as a dead end re-opens a KNOWN bypass:
                # `bash -c '<the suite>' bash .../run-artifact.sh lbl -- true`
                # runs the suite out of the body while the trailing words claim
                # the wrapper exemption for the segment.
                if j + 1 < len(toks):
                    inner = _match_suite_invocation(toks[j + 1])
                    if inner:
                        return inner.group(0)
                return None
            j += 1
        if j >= len(toks):
            return None
        if _SUITE_TOKEN_RE.search(toks[j]):
            return f"{base} {toks[j]}"
        head, i = toks[j], j      # e.g. `bash .../run-artifact.sh ...`
    elif _SUITE_TOKEN_RE.search(head):
        return head
    if base == "make":
        for t in toks[i + 1:]:
            if "=" in t:
                continue         # `make test=1` is a variable, not a target
            if _MAKE_TARGET_RE.match(t):
                return f"make {t}"
    # Recurse past `--` ONLY for the sanctioned wrapper. A blanket recursion
    # broke `git log --oneline -- scripts/test.sh`, where `--` separates git's
    # options from a PATHSPEC and the suite path is an argument, not a command.
    if head.rsplit("/", 1)[-1] == "run-artifact.sh" and "--" in toks[i:]:
        tail = toks[i:]
        rest = tail[tail.index("--") + 1:]
        if rest:
            return _tokens_invocation(rest)
    return None


def _match_suite_invocation(cmd: str):
    """First suite-script/make-test INVOCATION in `cmd`, else None.

    REWRITTEN 2026-07-30 (v04 runner item). The regex matcher pattern-matched a
    shell grammar it did not parse, and this was its THIRD false-positive shape:
    `_SCRIPT_RES[0]`'s `\\b` was satisfied by the dot in `*.sh` (fixed 77d36c59),
    then `_CMD_POS`'s `|` and `(` fired on quoted DATA -- live, twice, 7 hours
    apart in the 3-day canary: once on `(scripts/test.sh` inside prose, once on
    the ledger string `"high|scripts/test-tooling.sh:7883|..."`.

    The evasion was the real finding: the run learned to split a string literal
    (`TT="scripts/test-tooling"".sh:7883"`) to get a ledger write past the gate,
    after four consecutive blocked retries. A gate that teaches the model to
    obfuscate its own commands has stopped guarding and started shaping
    behaviour, so this is a rewrite rather than a fourth special case.

    Fail-SAFE throughout: an unbalanced quote or a shlex parse error yields NO
    match. A cost gate that cannot parse the command must never be the thing
    that stops a run -- the same lesson `_skip_env` paid for in 326bc786."""
    # SUBSTITUTION BODIES RUN. `$(...)`, backticks and `<(...)`/`>(...)` execute
    # their body as a command while being, syntactically, an ARGUMENT to the
    # surrounding one -- so an otherwise-exempt outer segment used to carry a
    # bare suite run along inside it. Checked FIRST, and against the raw command,
    # because a body inside double quotes never reaches segment splitting.
    for body in _substitution_bodies(cmd):
        inner = _match_suite_invocation(body)
        if inner:
            return inner
    segs = _split_unquoted(cmd)
    if segs is None:
        # UNPARSEABLE. The v04 item prescribed fail-OPEN here ("a cost gate that
        # cannot parse the command must not be the thing that stops a run"), and
        # that is right for the FALSE-POSITIVE class it was written about --
        # quoted data blocking benign work. It is WRONG for this class, and the
        # existing suite says so in as many words: "a false block costs a
        # rephrase; a false allow runs the suite with nothing reporting it."
        # Reconciled by which risk is actually present: a command we cannot parse
        # that also NAMES a suite script is a possible bypass and falls back to
        # the legacy textual matcher (fail-CLOSED, preserving the hardening that
        # test earned); one that names no suite at all has nothing to block, so
        # the fail-open intent is preserved where it applies.
        for rx in _SCRIPT_RES:
            m = rx.search(cmd)
            if m:
                return m
        return None
    for seg in segs:
        hit = _segment_invocation(seg)
        if hit:
            return _Hit(hit)
    return None

# `{matched}` carries the SEGMENT the matcher actually fired on, not just the
# script name. WHY (2026-07-31): a run filed this hook for blocking a heredoc
# whose body merely mentions the build script in prose. Three of the four cited
# blocks turned out to be genuine bare builds ("... and rebuild"); the fourth
# could not be judged either way, because the message named only `scripts/build.sh`
# and the run log clips the command -- so there was no way to tell a real
# invocation from a prose mention after the fact. Seven candidate prose shapes
# were tested against this hook and NONE reproduced a false block, so the
# matching is not being changed on unreproducible evidence; what is being fixed
# is the observability gap that made the report unfalsifiable. Same lesson as
# the R4 defect, which hid for three sections because nothing recorded what the
# gate matched.
_MSG = (
    "[build-offload BLOCK -- reroute] Overnight SECTIONS phase ran `{script}` "
    "BARE in the MAIN context (matched segment: `{matched}`). If that segment "
    "is NOT a real invocation (e.g. prose inside a heredoc body), this is a "
    "false positive worth filing WITH the segment text. Re-issue it through the DETERMINISTIC "
    "wrapper (NO model): `bash scripts/overnight/run-artifact.sh <label> -- "
    "{script}` returns a compact JSON envelope (verdict + error lines + "
    "artifact path) and tees full output to disk; you quote the tail "
    "yourself. A green run spends ZERO Sonnet -- dispatch diagnostic-digester "
    "ONLY on a FAIL envelope. Measured cost of the bare path: 91 in-context "
    "build/test runs floods the 2026-07-02 overnight context."
)

# P3.4 prereq 3: suppress a repeat reminder for the SAME script within this
# window -- the reminder fired 9x on one properly-wrapped build/test sequence
# before the exemption, and duplicate nags add churn without new signal.
_DEDUP_WINDOW_S = 300


def _recently_fired(root: Path, script: str) -> bool:
    """True if build_offload_reminder already fired for `script` within
    _DEDUP_WINDOW_S. Reads the shared offload-events.jsonl (newest first)."""
    try:
        p = root / ".claude" / "state" / "offload-events.jsonl"
        if not p.exists():
            return False
        now = time.time()
        for line in reversed(p.read_text(encoding="utf-8").splitlines()):
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except Exception:
                continue
            if (rec.get("hook") == "build_offload_reminder"
                    and rec.get("kind") == "fire"
                    and rec.get("detail") == script):
                ts = rec.get("ts")
                return isinstance(ts, (int, float)) and (now - ts) <= _DEDUP_WINDOW_S
        return False
    except Exception:
        return False


def _repo_root() -> Path | None:
    here = Path(__file__).resolve()
    for p in here.parents:
        if (p / ".claude").is_dir():
            return p
    return None


def _is_headless_run() -> bool:
    """True only inside the UNATTENDED run this gate governs.

    SESSION SCOPING (2026-07-28). `_in_sections()` reads the GLOBAL cursor and
    says nothing about who is issuing the command, so while a run sat in
    SECTIONS every interactive operator session in the repo inherited its phase
    gates -- including the BLOCK above, whose entire rationale ("91 in-context
    build/test runs floods the overnight context") is about the RUN's context
    budget, not an operator's. Observed live: an operator's read-only grep was
    blocked by a gate meant for the runner.

    The discriminator already exists and is used elsewhere for exactly this
    reason -- `run_phase_guard.handle_stop()` returns early on it so "an
    interactive operator session is never trapped". `OVERNIGHT_SEQUENCER_RUN=1`
    is set by the arm drop-in on the systemd unit and is never present in an
    operator shell.

    Deliberately the SAME single test as `run_phase_guard.is_headless()` and
    nothing more. A second, cleverer heuristic here would be a second answer to
    "is this the run?", and the two would drift.
    """
    return os.environ.get("OVERNIGHT_SEQUENCER_RUN") == "1"


def _in_sections(root: Path) -> bool:
    # Both conditions are required: the RUN must be in SECTIONS *and* this
    # session must BE the run. Either alone is not the gate's subject.
    if not _is_headless_run():
        return False
    try:
        st = json.loads(
            (root / ".claude" / "state" / "sequencer-run.json").read_text())
    except Exception:
        return False
    return bool(st.get("active")) and st.get("phase") == "SECTIONS"


def _recent_checks_runner_dispatch(root: Path) -> bool:
    try:
        st = json.loads(
            (root / ".claude" / "state" /
             "last-agent-dispatch.json").read_text())
    except Exception:
        return False
    by_type = st.get("by_type")
    entry = by_type.get("checks-runner") if isinstance(by_type, dict) else None
    ts = entry.get("timestamp_ns") if isinstance(entry, dict) else None
    return isinstance(ts, int) and (time.time_ns() - ts) <= FRESH_NS


def _exempt_segment(text: str) -> bool:
    """True when this ONE segment may name a suite script without invoking one.

    Two shapes qualify:

    - the SANCTIONED wrapped route (`run-artifact.sh <label> -- bash
      scripts/build.sh`), which legitimately contains the inner invocation this
      gate matches. Firing on it would BLOCK the very route the gate's own
      message recommends -- it fired 9x on one properly-wrapped sequence (Codex
      audit 2026-07-13, verified);
    - a CODEX DISPATCH, whose prompt has to NAME the files it reviews. An
      adversarial review of `scripts/test.sh` necessarily contains that string,
      and the gate matched it as an invocation and BLOCKED the mandatory review
      (observed 2026-07-30 reviewing orphaned-QEMU recovery, whose entire diff
      is in that script). Nothing is run: a dispatch execs the companion with
      ONE argument and `codex-dispatch.sh` enforces argc == 1.
    """
    if _RUN_ARTIFACT_RE.search(_normalise_continuations(text)):
        return True
    try:
        from _codex_dispatch import is_codex_dispatch
        return bool(is_codex_dispatch(text))
    except Exception:
        # A missing/renamed helper must not resurrect the false positive
        # silently, but it must not crash the gate either: fall back to the
        # narrow literal check.
        return bool(re.search(
            r"\breview-broker-codex-dispatch\.sh\b|\bcodex-dispatch\.sh\b"
            r"|\bcodex-companion\.mjs\b", text))


def _blocking_match(cmd: str):
    """The suite invocation that should BLOCK, or None if every one is exempt.

    Classification is PER SEGMENT. Exempting the whole command line on a
    whole-command answer is a BYPASS: `is_codex_dispatch` (and a bare
    `run-artifact.sh` substring search) is true when ANY segment qualifies, so
    `codex-dispatch.sh '<prompt>' && bash scripts/test.sh` carried a real
    dispatch AND a real bare suite run, and the dispatch alone excused both.
    Judging each segment on its own keeps the exemption exactly as wide as the
    thing it excuses.
    """
    segs = _codex_segments(cmd)
    if not segs:
        # UNSPLITTABLE -> FAIL CLOSED. The screen above already proved this
        # command contains a suite invocation somewhere; what we have lost is
        # the ability to say WHERE, and therefore any basis for excusing it.
        # Granting the whole-command exemption here is fail-open, and it was
        # reachable from valid shell: a backtick substitution containing `)`
        # inside a `$( )` desynchronised the walk, so a command carrying both a
        # Codex dispatch and a bare suite run split to nothing and was excused
        # wholesale. A false BLOCK costs a rephrase; a false ALLOW runs the
        # suite in the main context and nothing reports it.
        return _match_suite_invocation(cmd)
    for text, is_codex in segs:
        if is_codex or _exempt_segment(text):
            continue
        # `lstrip()` because a segment's START is a command position, and the
        # separator that produced it leaves the following space behind:
        # `cd /repo && scripts/test.sh SUITE=mm` splits to " scripts/test.sh
        # SUITE=mm", where the direct-exec detector's `^`-or-`[;&|(]` anchor no
        # longer sits on the script. The whole command used to satisfy that
        # anchor via the `&` itself, so segmenting silently un-matched it.
        m = _match_suite_invocation(text.lstrip())
        if m:
            return m
    return None


def _codex_segments(cmd: str):
    try:
        from _codex_dispatch import command_segments
        return command_segments(cmd)
    except Exception:
        return []


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict) or d.get("tool_name") != "Bash":
        return 0
    cmd = (d.get("tool_input") or {}).get("command") or ""
    # Cheap whole-command screen first: no suite shape anywhere means there is
    # nothing to segment or exempt.
    if not _match_suite_invocation(cmd):
        return 0
    root = _repo_root()
    if root is None or not _in_sections(root):
        return 0
    m = _blocking_match(cmd)
    if m is None:
        # R2: record the reroute as a `follow` event -- offload-report.py
        # counted only Agent dispatches as compliance, so the P3.4 reroute (a
        # Bash call, not an Agent) read as a 9% follow rate when the block was
        # in fact working (2026-07-19 measurement: all bare attempts blocked
        # pre-execution).
        if _RUN_ARTIFACT_RE.search(_normalise_continuations(cmd)):
            try:
                import _offload_log
                _offload_log.log_event(root, "follow", "build_offload_reminder",
                                       _match_suite_invocation(cmd).group(0))
            except Exception:
                pass
        return 0
    if _recent_checks_runner_dispatch(root):
        return 0
    # P3.4: BLOCK-with-reroute. Log once per window (dedup) but ALWAYS block --
    # a repeat bare attempt must keep being rerouted, so the dedup gates only the
    # offload-events log write, never the block decision.
    if not _recently_fired(root, m.group(0)):
        try:
            import _offload_log
            _offload_log.log_event(root, "fire", "build_offload_reminder", m.group(0))
        except Exception:
            pass
    _seg = (m.string[max(0, m.start() - 20):m.end() + 40]
            if getattr(m, "string", None) else m.group(0))
    _seg = " ".join(_seg.split())[:120]
    sys.stderr.write(_MSG.format(script=m.group(0), matched=_seg) + "\n")
    return 2


if __name__ == "__main__":
    sys.exit(main())
