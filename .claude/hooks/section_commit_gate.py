#!/usr/bin/env python3
"""Hard-block git commits that look like a TODO-section ship without
the build / Codex / receiving-review evidence the implement-todo-section
and verify-todo-section pipelines are required to produce.

Replaces the inline reminder-mode block previously inlined under
.claude/settings.json PreToolUse Bash. Today's reminder prints a long
systemMessage and exits 0; the user's feedback_never_skip_review
memory says reminder mode does not work. This script EXITS 2 when:

  1. The Bash invocation is a `git commit`, AND
  2. The pre-Bash staged diff carries the section-commit signature
     (source-code path + TODO Implementation Order [x] flip), AND
  3. Any of the three evidence pieces is missing or mismatched:
        a. build/build.log shows `=== BUILD OK ===` AND mtime is
           NEWER than every staged source file's mtime (catches
           "edited after build" without rebuild).
        b. .claude/state/last-codex-review.json shows
           received: true within the last 30 minutes AND the review's
           trigger_files cover EVERY staged source path.
        c. (baked into 2) the Codex review's `received: true` is
           only set by superpowers:receiving-code-review per §3.

Two execution modes:

  Default (harness PreToolUse):
    Reads the Claude Code hook payload from stdin (JSON with
    tool_name + tool_input.command). Detects `git commit` segments
    via shlex tokenization + segment-by-control-operators + wrapper
    walk. The §3 hook (codex_review_completed.py) is the canonical
    source of that pattern; this module imports it.

  --git-hook-mode (called from .githooks/pre-commit):
    No stdin payload. Trigger condition is "we are inside `git
    commit`"; signature detection runs against the staged index that
    git is about to commit. Catches the harness TOCTOU bypass
    `git add foo && git commit -m bar` -- the harness PreToolUse
    sees staging BEFORE `git add` runs, but the .githooks pre-commit
    sees the FINAL index.

Opt-out (BOTH required):

  SKIP_REVIEW_HOOK=1
  SKIP_REVIEW_HOOK_REASON="<plain-language reason, >= 12 chars>"

  On opt-out the gate appends a JSONL audit record to
  .claude/state/skip-log.jsonl AND resets last-codex-review.json so
  the next commit cannot reuse the same review state. Codex M1:
  "skip is a state transition, not an audit append."

Fail behavior split (Codex H2):

  Pre-signature:    fail OPEN on errors (malformed payload, git not
                    on PATH, regex error). The gate is a hard block
                    only when we know we're looking at a section
                    commit; before that, errors must not block
                    unrelated tool calls.
  Post-signature:   fail CLOSED on missing or malformed evidence.
                    Once we know this IS a section commit, missing
                    state is missing evidence, not allow.

Owner: TODO-08-automation-hardening section 4.
"""

import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

# Reuse the §3 hook's helpers. The gate's signature detection mirrors
# its segment-by-control-operators + wrapper-walk pattern, and we
# share the source-path classifier so trigger_files / staged_files
# overlap deterministically.
#
# Codex perf finding (round-7 review-pipeline): import was at
# module-load and added ~36ms per Bash hook invocation, even for
# obvious non-commit commands like `ls`. Now lazy: PreToolUse Bash
# hooks return early via _looks_like_git_commit_screen() without
# importing crc at all. The expensive import only happens when the
# command MAY be a git commit. Module-level reference is set on
# first lazy load to avoid repeat resolution.
_HOOK_DIR = Path(__file__).resolve().parent
crc = None  # populated lazily by _ensure_crc()


def _ensure_crc():
    """Lazy-import codex_review_completed only when needed."""
    global crc
    if crc is None:
        if str(_HOOK_DIR) not in sys.path:
            sys.path.insert(0, str(_HOOK_DIR))
        import codex_review_completed as _crc  # noqa: E402
        crc = _crc
    return crc


def _looks_like_git_commit_screen(cmd: str) -> bool:
    """Fast literal screen: skip the full shlex+wrapper-walk for
    commands that obviously can't be a git commit. Returns True if
    the command MIGHT be a git commit, False if definitely not.
    Cost: 2-3 string searches, no allocations beyond the input.

    The screen is intentionally permissive (false positives are OK
    -- they pay the full parse cost; false negatives are NOT OK).
    Conservative: any command containing the substring `git` is
    forwarded; commands without `git` cannot be a git invocation
    no matter how wrapped.
    """
    if not isinstance(cmd, str) or not cmd:
        return False
    # Cheap literal -- false-positive on `digit`, `gita`, etc., but
    # those just pay the full parse cost. False-negative on a custom
    # git binary symlinked under a non-git name is the only real risk;
    # we accept it (the user opting out of `git` as a name is signing
    # up for non-standard tooling and the gate doesn't pretend to
    # cover it).
    return "git" in cmd

# ----------------------------------------------------------------------
# Constants
# ----------------------------------------------------------------------

# LEGACY-FALLBACK ONLY (content-addressed receipts, 2026-07-11): evidence
# validity is content-bound -- a build receipt matches the current build-input
# fingerprint, a review binds staged blob SHAs, a dispatch binds its HEAD with
# no source drift since. These wall-clock TTLs apply ONLY when the content
# binding is absent (pre-receipt build.log, legacy dispatch entries without a
# `<kind>_head`). A slow review over unchanged content never expires.
EVIDENCE_TTL_SECONDS = 30 * 60  # legacy fallback for build evidence (no receipt)
FOUR_DISPATCH_TTL_SECONDS = 30 * 60  # legacy fallback (dispatch entry lacks head)
RE_ADV_TRIGGER_TTL_SECONDS = 30 * 60  # legacy fallback (re-adv entry lacks head)
SKIP_REASON_MIN_LEN = 12

# Paths whose post-dispatch drift invalidates a recorded Codex dispatch: code
# the review looked at. TODO/doc edits (the review's own fix-loop and stamp
# edits) do NOT invalidate. Mirrors receipts.BUILD_INPUT_PATHS minus config.
_DISPATCH_CONTENT_PATHS = ("src/", "include/", "user/", "tools/")

# Dispatch kinds tracked in last-review-stamps.json (§5). Dead-code
# was retired in review-todo-section/SKILL.md on 2026-04-25; the
# canonical step-8 dispatch set is three (adversarial / consistency
# / perf). The TODO-08 §5 text predates the retirement and uses a
# "FOUR" framing; this implementation reflects current SKILL.md.
_DISPATCH_KINDS = ("adversarial", "consistency", "perf")

# TODO-08 §17 re-adversarial trigger detection. Step 13.5 of
# review-todo-section requires a re-adversarial Codex dispatch when
# the cumulative fix diff matches one of these triggers. Per Codex
# design review 2026-04-28 M3, the regexes match the locking /
# faultable / lifecycle vocabulary actually used in this codebase.
# State-machine and >3-functions triggers are NOT mechanically
# detectable here -- documented as manual-only gaps; review-todo-
# section step 13.5 prose still names them, and the agent must
# dispatch re-adversarial by hand when those fire.
_RE_ADV_TRIGGER_LOCKING = re.compile(
    # TODO-08 §17 deferred-XREF M3 fix: expanded vocab covers all
    # in-tree synchronization primitives + direct __atomic_* ops.
    # Codex consistency review identified rwlock_t (sched/rwlock.h),
    # seqlock_t (sched/seqlock.h), semaphore_t (sched/semaphore.h),
    # and __atomic_* operations (time/wall_clock.c, exec.c) as missed.
    r"\b(?:spinlock_t|atomic_t|atomic\d+_t|mutex_t|rwlock_t|seqlock_t|semaphore_t)\b"
    r"|\b__atomic_\w+"
)
_RE_ADV_TRIGGER_FAULTABLE = re.compile(
    r"\b(?:boot_halt|panic|KeBugCheck\w*|fault_handler|exception_\w*)"
)
_RE_ADV_TRIGGER_LIFECYCLE = re.compile(
    r"\b\w*_(?:alloc|free|refcount)\b"
)
_RE_ADV_LOC_THRESHOLD = 50  # >50 LOC C/H trigger

# Stamp lines whose presence in the staged diff triggers the
# four-dispatch check. `^\+\s{0,3}(?:>\s*)+\*\*` matches ADDED stamp
# lines with 0-3 leading spaces of valid Markdown blockquote indent
# AND tolerates nested blockquote markers (`> > **`, `>> **`).
# Unchanged context lines starting with `>` (no leading `+`) do not
# trigger. Codex M4 (review-pipeline post-impl): regex without
# leading-space tolerance was evadable; Codex post-commit review_C:
# the single-`>` form was also evadable via Markdown-valid nested
# blockquote forms (`+> > **Verified:**` / `+>> **Quality reviewed:**`).
_STAMP_ADDED_RE = re.compile(
    r"^\+\s{0,3}(?:>\s*)+\*\*(Verified|Quality reviewed):\*\*",
)
# Companion regex: REMOVED stamp line carrying the same label. When a
# diff hunk has BOTH a removed `> **Verified:**` AND an added
# `> **Verified:**`, that's an in-place UPDATE of an existing stamp
# (e.g. counts changed during a gap-audit follow-up), not a fresh
# section ship. The four-dispatch check should not fire on updates.
_STAMP_REMOVED_RE = re.compile(
    r"^-\s{0,3}(?:>\s*)+\*\*(Verified|Quality reviewed):\*\*",
)

# Wrapper / env tokens we walk past to find the real `git` command.
# Hardcoded (round-7 perf fix) instead of importing from crc to avoid
# the ~36ms module-load cost on every PreToolUse Bash. Mirrors
# crc._CODEX_WRAPPER_TOKENS plus xargs (commit-runner specific to
# section-commit detection).
_WRAPPER_TOKENS = frozenset({
    "sudo", "doas", "env", "nice", "nohup", "timeout", "ionice",
    "stdbuf", "unbuffer", "chronic", "exec", "command",
    "taskset", "chrt", "setsid", "setpriv", "cgexec", "flock",
    "xargs",
})

# Implementation Order row: status column is the FINAL `|`-bounded
# cell. Anchor regex to the END of the line so `[x]` in description
# cells (Codex H1) does not false-trigger.
#
# A diff line that REMOVES a row in `[ ]` or `[/]` state:
_IO_OLD_OPEN_RE = re.compile(
    r"^-\s*\|.+?\|\s*\[(?: |/)\]\s*\|\s*$",
)
# A diff line that ADDS a row in `[x]` state:
_IO_NEW_DONE_RE = re.compile(
    r"^\+\s*\|.+?\|\s*\[x\]\s*\|\s*$",
)
# DEMOTION pair: REMOVED `[x]` row + ADDED `[ ]` or `[/]` row in the
# same hunk. This is the opposite direction from a section ship --
# typical sources are a gap-audit follow-up that downgrades a paper-
# complete section after finding a real gap, OR a revert of a
# previously-stamped section. Demotions must not require fresh
# adversarial+consistency+perf dispatches; the four-dispatch check is
# a promotion gate.
_IO_OLD_DONE_RE = re.compile(
    r"^-\s*\|.+?\|\s*\[x\]\s*\|\s*$",
)
_IO_NEW_OPEN_RE = re.compile(
    r"^\+\s*\|.+?\|\s*\[(?: |/)\]\s*\|\s*$",
)
_HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@")


# ----------------------------------------------------------------------
# Detection: is this Bash command a `git commit`?
# ----------------------------------------------------------------------


def _segment_is_git_commit(seg_tokens: list[str]) -> tuple[bool, bool]:
    """Returns (is_git_commit, has_no_verify). Walks env-prefix +
    wrapper tokens, then handles git's own pre-subcommand options
    (`-c key=value`, `-C path`, `--git-dir=...`, etc.) before
    matching the subcommand. Also flags `--no-verify`/`-n` (Codex
    H1: --no-verify skips .githooks/pre-commit entirely, so the
    harness layer must catch it).
    """
    out = list(seg_tokens)
    # Strip env-prefix assignments.
    while out and "=" in out[0] and not out[0].startswith("="):
        head = out[0].split("=", 1)[0]
        if head and (head[0].isalpha() or head[0] == "_") and all(
            c.isalnum() or c == "_" for c in head
        ):
            out.pop(0)
        else:
            break
    # Codex H1c + H2 round-2 + round-3: descend into `bash -c "..."` /
    # `bash -lc "..."` / `bash --rcfile myrc -c "..."` / `sh -ec ...`
    # so combined short flags AND long-flag-with-value forms don't
    # bypass. Any combined short flag containing 'c' (e.g. `-lc`, `-ec`,
    # `-cl`) acts as the script marker; the script is the next
    # positional arg.
    _SHELL_LONG_VALUE_FLAGS = frozenset({"--rcfile", "--init-file",
                                          "--noprofile-file", "-O"})
    if len(out) >= 3 and out[0].rsplit("/", 1)[-1] in ("bash", "sh", "zsh", "dash"):
        idx = 1
        found_c = False
        while idx < len(out) and out[idx].startswith("-"):
            flag = out[idx]
            if flag == "-c":
                found_c = True
                idx += 1
                break
            # Combined short flag: `-lc`, `-ec`, `-cl`. Long flags
            # (`--login`) don't carry the c semantic.
            if not flag.startswith("--") and len(flag) > 1 and "c" in flag[1:]:
                found_c = True
                idx += 1
                break
            # Long-flag-with-value forms: `--rcfile myrc`. Skip the
            # value too. The `=value` attached form is single-token.
            if flag in _SHELL_LONG_VALUE_FLAGS and "=" not in flag:
                idx += 2
                continue
            idx += 1
        if found_c and idx < len(out):
            inner = out[idx]
            try:
                inner_toks = shlex.split(inner, posix=True, comments=False)
            except ValueError:
                inner_toks = []
            for inner_seg in crc._segment_by_separators(inner_toks):
                inner_seg = crc._trim_heredoc_body(inner_seg)
                if not inner_seg:
                    continue
                is_inner, inner_no_verify = _segment_is_git_commit(inner_seg)
                if is_inner:
                    return (True, inner_no_verify)
            return (False, False)
    # `eval "git commit --no-verify -m foo"` -- eval re-shells its arg.
    # Concatenate all positional args (eval joins with spaces in POSIX
    # shells) and re-tokenize. Codex H2 round-4 fix.
    if out and out[0].rsplit("/", 1)[-1] == "eval":
        joined = " ".join(out[1:])
        if joined.strip():
            try:
                inner_toks = shlex.split(joined, posix=True, comments=False)
            except ValueError:
                inner_toks = []
            for inner_seg in crc._segment_by_separators(inner_toks):
                inner_seg = crc._trim_heredoc_body(inner_seg)
                if not inner_seg:
                    continue
                is_inner, inner_no_verify = _segment_is_git_commit(inner_seg)
                if is_inner:
                    return (True, inner_no_verify)
        return (False, False)
    # Walk wrappers using a per-wrapper value-flag table (Codex H2
    # round-3 + round-4): `env -u FOO git ...` and `sudo -u alice
    # git ...` and `timeout -k 5 30 git ...` all need the value-bearing
    # flag to be consumed as a value, not left as a positional arg
    # masquerading as the command head. Round-4 fix: drop NO-VALUE
    # flags from each wrapper so they don't pop the next positional
    # (which would be the real command). Specifically removed:
    #   sudo -n        (--non-interactive, no value)
    #   command -p     (use default PATH, no value; only flag)
    #   ionice -t      (ignore-failure, no value)
    _WRAPPER_VALUE_FLAGS = {
        "sudo": frozenset({"-u", "-g", "-p", "-r", "-h", "-D", "-C"}),
        "doas": frozenset({"-u", "-C"}),
        "timeout": frozenset({"-k", "--kill-after", "-s", "--signal"}),
        "nice": frozenset({"-n", "--adjustment"}),
        "ionice": frozenset({"-c", "--class", "-n", "--classdata",
                              "-p", "--pid", "-P", "-u"}),
        "env": frozenset({"-u", "--unset", "-S", "--split-string",
                          "-C", "--chdir"}),
        "stdbuf": frozenset({"-i", "-o", "-e"}),
        "command": frozenset(),
        "taskset": frozenset({"-c", "--cpu-list", "-p", "--pid"}),
        "chrt": frozenset({"-p", "--pid"}),
        # flock: -c/--command is a SHELL-DESCENT flag, not a generic
        # value-flag -- handled in the wrapper-specific block below.
        "flock": frozenset({"-w", "--timeout", "-E", "--conflict-exit-code"}),
        # setpriv per `setpriv --help` (util-linux 2.39+). Round-5
        # added: --ruid/--euid/--rgid/--egid (current util-linux names),
        # --securebits, --pdeathsig, --selinux-label, --apparmor-profile.
        # --reuid/--regid kept as legacy aliases. No-value:
        # --clear-groups, --keep-groups, --init-groups, --reset-env.
        "setpriv": frozenset({"--reuid", "--regid",
                              "--ruid", "--euid", "--rgid", "--egid",
                              "--groups", "--inh-caps", "--ambient-caps",
                              "--bounding-set", "--securebits",
                              "--pdeathsig", "--selinux-label",
                              "--apparmor-profile"}),
        # cgexec round-5 fix: --sticky is NO-VALUE (synopsis is
        # `cgexec [-g controllers:path] [--sticky] command`).
        "cgexec": frozenset({"-g"}),
        # bash builtin `exec [-cl] [-a name] command ...`. Only -a
        # takes a value.
        "exec": frozenset({"-a"}),
        # `xargs -I _ -n 1 git commit ...` -- xargs is a command-runner
        # that effectively chains a positional command after its flags.
        "xargs": frozenset({"-I", "--replace", "-i",
                            "-d", "--delimiter",
                            "-E",
                            "-L", "--max-lines",
                            "-n", "--max-args",
                            "-P", "--max-procs",
                            "-s", "--max-chars",
                            "-a", "--arg-file",
                            "--process-slot-var"}),
    }

    def _looks_like_cpu_mask(tok: str) -> bool:
        """taskset MASK accepts hex (0x prefix or bare hex digits) or
        decimal. Allow comma/dash for safety though those are -c forms.
        """
        if not tok:
            return False
        s = tok.lower()
        if s.startswith("0x"):
            return all(c in "0123456789abcdef" for c in s[2:]) and len(s) > 2
        return all(c in "0123456789abcdef" for c in s) and any(c.isdigit() for c in s)

    def _is_duration(tok: str) -> bool:
        if not tok:
            return False
        if tok[0].isdigit():
            if tok[-1] in "smhd" and len(tok) > 1:
                return tok[:-1].replace(".", "", 1).isdigit()
            return tok.replace(".", "", 1).isdigit()
        return False

    while out and out[0] in _WRAPPER_TOKENS:
        wrapper = out.pop(0)
        # flock special-case (Codex H2 round-4): `flock -c "cmd" file`
        # OR `flock file -c "cmd"` runs cmd as a shell. `flock -n file
        # cmd args` runs cmd with args. Both forms have a lockfile/fd
        # positional that must NOT be treated as the command head.
        if wrapper == "flock":
            inner_cmd = None
            consumed_lockfile = False
            while out and (out[0].startswith("-") or not consumed_lockfile):
                if out[0].startswith("-"):
                    flag = out.pop(0)
                    if "=" in flag:
                        head_eq, val_eq = flag.split("=", 1)
                        if head_eq in ("-c", "--command"):
                            inner_cmd = val_eq
                        continue
                    if flag in ("-c", "--command") and out:
                        inner_cmd = out.pop(0)
                        continue
                    if flag in ("-w", "--timeout", "-E",
                                "--conflict-exit-code") and out:
                        out.pop(0)
                        continue
                    # bare flock flags: -s/-x/-u/-n/-o/-F/-E/-h/-V (no value)
                    continue
                # Non-flag positional. First positional is the lockfile
                # or fd; consume it once, then exit so the real command
                # head can be processed by the outer logic.
                if not consumed_lockfile:
                    out.pop(0)
                    consumed_lockfile = True
                    continue
                break
            if inner_cmd is not None:
                try:
                    inner_toks = shlex.split(inner_cmd, posix=True, comments=False)
                except ValueError:
                    inner_toks = []
                for inner_seg in crc._segment_by_separators(inner_toks):
                    inner_seg = crc._trim_heredoc_body(inner_seg)
                    if not inner_seg:
                        continue
                    is_inner, inner_no_verify = _segment_is_git_commit(inner_seg)
                    if is_inner:
                        return (True, inner_no_verify)
                # -c was used; no further positional command follows.
                return (False, False)
            continue  # flock with no -c -- proceed to next wrapper iter
        value_flags = _WRAPPER_VALUE_FLAGS.get(wrapper, frozenset())
        while out and out[0].startswith("-"):
            flag = out.pop(0)
            if "=" in flag:
                continue
            if flag in value_flags and out:
                out.pop(0)
        # Wrapper-specific positional args before the real command.
        if wrapper == "timeout" and out and _is_duration(out[0]):
            out.pop(0)
        elif wrapper == "taskset" and out and _looks_like_cpu_mask(out[0]):
            # `taskset MASK cmd args` -- MASK is hex/decimal bitmask
            # (e.g. 03, 0x3, ff). With -p it's a PID after -p flag
            # (already consumed). With -c it's a list after -c (also
            # already consumed). Round-5 fix.
            out.pop(0)
        elif wrapper == "env":
            # `env FOO=bar BAZ=qux git commit ...` -- consume KEY=VAL pairs.
            while out and "=" in out[0] and not out[0].startswith("="):
                head_a = out[0].split("=", 1)[0]
                if head_a and (head_a[0].isalpha() or head_a[0] == "_") and all(
                    c.isalnum() or c == "_" for c in head_a
                ):
                    out.pop(0)
                else:
                    break
        elif wrapper == "nice" and out and out[0].lstrip("-").isdigit():
            out.pop(0)
        elif wrapper == "chrt" and out and out[0].isdigit():
            out.pop(0)
    if not out:
        return (False, False)
    head = out[0].rsplit("/", 1)[-1]
    if head != "git":
        return (False, False)
    # Walk past git's own pre-subcommand options. Capture inline
    # `-c alias.<name>=<value>` definitions so we can detect commit
    # aliases the user defined on the command line (Codex round-6
    # post-commit fix: `git -c alias.ship='commit --no-verify' ship`
    # was a hard bypass of the gate).
    idx = 1
    git_value_flags = ("-c", "-C", "--git-dir", "--work-tree",
                        "--namespace", "--exec-path", "--super-prefix")
    inline_aliases: dict[str, str] = {}

    def _maybe_record_alias(kv: str) -> None:
        # `kv` is the value of `-c key=value` (with `=` already split).
        if "=" not in kv:
            return
        key, _, val = kv.partition("=")
        if key.startswith("alias."):
            inline_aliases[key[len("alias."):]] = val

    while idx < len(out):
        tok = out[idx]
        if not tok.startswith("-"):
            break
        if tok in git_value_flags:
            if "=" in tok:
                # `-c=foo=bar` form (rare; `-c` actually parses as
                # `-c <next>` per git-c(1), but be defensive).
                _, _, attached = tok.partition("=")
                if tok.startswith("-c="):
                    _maybe_record_alias(attached)
                idx += 1
            else:
                # `-c key=value` -- next token is the value.
                if tok == "-c" and idx + 1 < len(out):
                    _maybe_record_alias(out[idx + 1])
                idx += 2
            continue
        # `-c=key=value` attached form variant: `--<flag>=value`.
        for f in git_value_flags:
            if tok.startswith(f + "="):
                if f == "-c":
                    _maybe_record_alias(tok[len(f) + 1:])
                idx += 1
                break
        else:
            idx += 1
    subcmd = out[idx] if idx < len(out) else ""
    # Codex H2 round-2 + round-6: subcommand allowlist + custom-alias
    # resolution. `commit`/`ci`/`cm` are the conventional commit names;
    # any user alias defined inline via `-c alias.<X>=<Y>` whose value
    # starts with `commit` (after optional flags) is treated as a commit
    # invocation, with the alias's value scanned for `--no-verify`.
    extra_no_verify = False
    if subcmd in inline_aliases:
        alias_value = inline_aliases[subcmd]
        # Round-7 fix: git supports SHELL aliases (leading `!`) where
        # the body is an arbitrary shell command, not a git subcommand.
        # `git -c alias.ship='!git commit --no-verify "$@"' ship` is a
        # documented and common pattern. Recurse into the shell body
        # via the same _bash_is_git_commit machinery used for `bash -c`
        # / `eval` shell descents.
        stripped = alias_value.lstrip()
        if stripped.startswith("!"):
            shell_body = stripped[1:]
            try:
                inner_toks = shlex.split(shell_body, posix=True, comments=False)
            except ValueError:
                inner_toks = []
            # Run each control-operator-separated segment through the
            # same recognizer; if any segment is a git commit invocation,
            # treat the outer alias as commit and inherit no-verify.
            for inner_seg in crc._segment_by_separators(inner_toks):
                inner_seg = crc._trim_heredoc_body(inner_seg)
                if not inner_seg:
                    continue
                is_inner, inner_no_verify = _segment_is_git_commit(inner_seg)
                if is_inner:
                    subcmd = "commit"
                    if inner_no_verify:
                        extra_no_verify = True
                    break
            # Also handle `!f() { git commit ...; }; f` shape: scan the
            # raw shell body string for `commit` + `--no-verify` literal
            # as a defense-in-depth backstop. Function-defining aliases
            # are uncommon in practice, but they exist.
            if subcmd not in ("commit", "ci", "cm"):
                if " commit" in shell_body or shell_body.startswith("commit"):
                    if "--no-verify" in shell_body or " -n " in (" " + shell_body + " "):
                        subcmd = "commit"
                        extra_no_verify = True
        else:
            try:
                alias_toks = shlex.split(alias_value, posix=True, comments=False)
            except ValueError:
                alias_toks = []
            # Round-8 fix: alias body can carry git pre-subcommand
            # options (`-c key=value`, `-C path`, `--git-dir=...`)
            # before the actual subcommand. Mirror the outer git
            # pre-subcommand walker -- value-bearing options consume
            # the next token, attached `--flag=value` consume one.
            a_idx = 0
            while a_idx < len(alias_toks):
                tok = alias_toks[a_idx]
                if not tok.startswith("-"):
                    break
                if tok in git_value_flags:
                    if "=" in tok:
                        a_idx += 1
                    else:
                        a_idx += 2
                    continue
                if any(tok.startswith(f + "=") for f in git_value_flags):
                    a_idx += 1
                    continue
                a_idx += 1
            alias_subcmd = alias_toks[a_idx] if a_idx < len(alias_toks) else ""
            if alias_subcmd in ("commit", "ci", "cm"):
                subcmd = "commit"
                for tok in alias_toks[a_idx + 1:]:
                    if tok in ("--no-verify", "-n") or tok.startswith("--no-verify="):
                        extra_no_verify = True
                        break
            if not extra_no_verify:
                for tok in alias_toks[:a_idx]:
                    if tok in ("--no-verify", "-n") or tok.startswith("--no-verify="):
                        extra_no_verify = True
                        break
    if subcmd not in ("commit", "ci", "cm"):
        return (False, False)
    # Codex H1: scan post-subcommand args for --no-verify / -n. The
    # `-n` short form is ambiguous (could be a value-arg of an
    # earlier flag), but in `git commit` argv positions after the
    # subcommand it is unambiguously --no-verify per `git-commit(1)`.
    has_no_verify = extra_no_verify
    if not has_no_verify:
        for tok in out[idx + 1:]:
            if tok in ("--no-verify", "-n"):
                has_no_verify = True
                break
            if tok.startswith("--no-verify="):
                has_no_verify = True
                break
    return (True, has_no_verify)


def _bash_is_git_commit(cmd: str) -> tuple[bool, bool]:
    """Returns (is_git_commit, has_no_verify) for any segment of
    `cmd`. Same shape as crc._is_codex_bash_trigger -- per-segment
    heredoc trim, segment-by-operator, scan each segment. Also
    handles inner `bash -c "..."` shells via _segment_is_git_commit.
    """
    if not isinstance(cmd, str) or not cmd.strip():
        return (False, False)
    try:
        toks = shlex.split(cmd, posix=True, comments=False)
    except ValueError:
        return (False, False)
    if not toks:
        return (False, False)
    saw_commit = False
    saw_no_verify = False
    for seg in crc._segment_by_separators(toks):
        seg = crc._trim_heredoc_body(seg)
        if not seg:
            continue
        is_commit, no_verify = _segment_is_git_commit(seg)
        if is_commit:
            saw_commit = True
            if no_verify:
                saw_no_verify = True
                break  # short-circuit; no_verify is the strongest signal
    return (saw_commit, saw_no_verify)


# ----------------------------------------------------------------------
# Detection: does the staged diff carry the section-commit signature?
# ----------------------------------------------------------------------


def _staged_source_files(root: Path) -> list[str]:
    """Same definition as crc._staged_source_files (which we import).
    Wrapped here for readability at the call sites.
    """
    return crc._staged_source_files(root)


def _index_worktree_desync(root: Path, paths: list[str]) -> list[str]:
    """Return paths whose worktree content differs from the index.
    `git diff --name-only -- <paths>` (no --cached) compares the
    worktree to the index; any path in output is desynced. On error
    we treat all paths as desynced (post-signature fail-closed).
    """
    if not paths:
        return []
    try:
        out = subprocess.check_output(
            ["git", "diff", "--name-only", "-z", "--", *paths],
            cwd=str(root), text=True, timeout=5,
            stderr=subprocess.DEVNULL,
        )
    except Exception:
        # Fail-closed inside the post-signature evidence path.
        return list(paths)
    return [p for p in out.split("\x00") if p]


def _staged_todo_files(root: Path) -> list[str]:
    try:
        out = subprocess.check_output(
            ["git", "diff", "--cached", "--name-only", "-z"],
            cwd=str(root), text=True, timeout=3, stderr=subprocess.DEVNULL,
        )
    except Exception:
        return []
    paths = [p for p in out.split("\x00") if p]
    return [p for p in paths if p.startswith("todo/") and p.endswith(".md")]


def _detect_signature(root: Path) -> tuple[str, list[str], list[str], dict[str, list[str]]]:
    """Returns (state, staged_source_files, flipped_or_stamped_todo_files,
    flipped_sections_by_todo) where
    state is one of:
      "section"     -- source + Implementation Order row flip; full evidence
                       (build + Codex/receiving + four-dispatch-if-stamped)
      "stamp_only"  -- TODO file(s) staged with ADDED stamp line(s) but no
                       source change. Codex M1 (post-impl): a separate
                       `stamp:` commit (the user's actual workflow per
                       git log) bypassed the four-dispatch check entirely
                       under the original §4 signature; this state path
                       runs ONLY the four-dispatch check (no build /
                       review evidence, since no source was reviewed).
      "not_section" -- not a section / stamp commit (allow)
      "unknown"     -- could not determine (Codex H2: in --git-hook-mode
                       this fails CLOSED if the index is non-empty,
                       because the load-bearing layer cannot fail open
                       on a real commit it failed to inspect).

    The unknown state is reached when:
      - The index is non-empty AND
      - `git diff --cached --name-only` succeeded but a downstream
        per-TODO `git diff` failed (timeout/error), so we know there
        IS a commit happening but cannot see its full shape.
    """
    src = _staged_source_files(root)
    todos = _staged_todo_files(root)
    if not src and not todos:
        # Empty index AND no error -- this is a commit attempt
        # against staging that has no source / no TODO. Cannot be a
        # section commit by definition.
        return ("not_section", src, [], {})
    if src and todos:
        flipped, todo_diff_ok, flipped_sections = _impl_order_flips_with_status(root, todos)
        if flipped:
            # Section signature: caller pulls all_staged_todos via
            # _staged_todo_files() to scan stamps in non-flipped TODOs
            # too (Codex M5 post-impl: cross-TODO stamp piggyback).
            return ("section", src, flipped, flipped_sections)
        if not todo_diff_ok:
            # Some per-TODO diff failed to read. Cannot rule out a flip.
            return ("unknown", src, [], {})
        # Codex M5 (post-impl): source + TODO without row flip but
        # WITH added stamps was a real bypass under the original
        # signature (`not_section` allowed it). Detect stamp adds and
        # route to stamp_only when present. Source files staged here
        # are context (e.g. doc/test/skill changes alongside a stamp);
        # the four-dispatch check fires for the stamped TODOs.
        try:
            stamped = _stamped_todos(root, todos)
        except Exception:
            return ("unknown", src, [], {})
        if stamped:
            return ("stamp_only", src, stamped, {})
        # Source + TODO without flip and without stamps: not a
        # section/stamp commit. Allow (prior behavior).
        return ("not_section", src, [], {})
    if todos and not src:
        # Stamp-only path (Codex M1 post-impl). Detect ADDED stamp
        # lines via the same _stamped_todos used by the four-dispatch
        # check. Per-file diff failure here is treated as "unknown"
        # only when the index is non-empty (caller handles that mode).
        try:
            stamped = _stamped_todos(root, todos)
        except Exception:
            return ("unknown", src, [], {})
        if stamped:
            return ("stamp_only", src, stamped, {})
        return ("not_section", src, [], {})
    # src and not todos: pure source change without TODO update; not a
    # section commit per the §4 contract.
    return ("not_section", src, [], {})


def _normalize_section_id(raw: object) -> str:
    text = str(raw or "").strip()
    text = text.lstrip("§").strip()
    m = re.fullmatch(r"(?:[sS]|section\s+)?(\d+)", text, re.IGNORECASE)
    if m:
        return m.group(1)
    return text


def _table_cells(line: str) -> list[str]:
    text = line.strip()
    if not text.startswith("|") or not text.endswith("|"):
        return []
    return [cell.strip() for cell in text.strip("|").split("|")]


def _table_labels(cells: list[str]) -> list[str]:
    return [re.sub(r"\s+", " ", cell.strip().lower()) for cell in cells]


def _section_value(raw: object) -> str:
    text = str(raw or "").strip()
    m = re.fullmatch(r"§\s*(\d+)", text)
    if m:
        return m.group(1)
    m = re.fullmatch(r"(?:[sS]|section\s+)?(\d+)", text, re.IGNORECASE)
    if m:
        return m.group(1)
    return ""


def _staged_file_lines(root: Path, path: str) -> list[str]:
    try:
        out = subprocess.check_output(
            ["git", "show", f":{path}"],
            cwd=str(root),
            text=True,
            stderr=subprocess.DEVNULL,
            timeout=5,
        )
    except Exception:
        return []
    return out.splitlines()


def _io_header_for_added_line(root: Path, todo: str, added_lineno: int) -> list[str]:
    lines = _staged_file_lines(root, todo)
    if not lines:
        return []
    start = min(max(added_lineno - 1, 0), len(lines) - 1)
    for idx in range(start, -1, -1):
        if re.match(r"^##+\s+", lines[idx]):
            break
        cells = _table_cells(lines[idx])
        labels = _table_labels(cells)
        if "status" in labels and ("order" in labels or "section" in labels):
            return cells
    return []


def _io_row_section(root: Path, todo: str, added_lineno: int, diff_line: str) -> str:
    if not _IO_NEW_DONE_RE.match(diff_line):
        return ""
    cells = _table_cells(diff_line[1:])
    if not cells or cells[-1] != "[x]":
        return ""
    header = _io_header_for_added_line(root, todo, added_lineno)
    labels = _table_labels(header)
    for wanted in ("section", "order"):
        if wanted in labels:
            idx = labels.index(wanted)
            if idx < len(cells):
                return _section_value(cells[idx])
            return ""
    numeric_cells = [cell for cell in cells if re.fullmatch(r"\d+", cell)]
    if numeric_cells:
        return numeric_cells[0]
    return ""


def _promoted_sections_for_hunk(
    root: Path,
    todo: str,
    added_done: list[tuple[int, str]],
    removed_done: list[tuple[int, str]],
) -> list[str]:
    added_sections = [
        sec
        for sec in (
            _io_row_section(root, todo, lineno, row)
            for lineno, row in added_done
        )
        if sec
    ]
    removed_sections = [
        sec
        for sec in (
            _io_row_section(root, todo, lineno, row)
            for lineno, row in removed_done
        )
        if sec
    ]
    for sec in removed_sections:
        try:
            added_sections.remove(sec)
        except ValueError:
            pass
    return added_sections


def _impl_order_flips_with_status(root: Path, todo_files: list[str]) -> tuple[list[str], bool, dict[str, list[str]]]:
    """Wrap _impl_order_flips with a success flag. Returns
    (flipped, all_diffs_succeeded, flipped_sections_by_todo). Used by
    _detect_signature to
    distinguish "no flip detected because no diff had one" from
    "no flip detected because git couldn't return the diff".

    PROMOTION ONLY: only counts `[ ]`/`[/]` -> `[x]` transitions.
    DEMOTION (`[x]` -> `[ ]`/`[/]`) is the opposite direction --
    typical sources are gap-audit follow-ups that downgrade a
    paper-complete section after finding a real gap. Demotions
    must not require fresh adversarial+consistency+perf dispatches;
    the four-dispatch gate is a promotion gate.

    Detection logic is section-set based. A hunk can contain unrelated
    demotions, edits to already-complete rows, and promotions at the same
    time, so row-count deltas are not enough. Flag any section that appears
    as newly `[x]` after subtracting sections that were already `[x]` in
    the same hunk.
    """
    flips: list[str] = []
    sections_by_todo: dict[str, list[str]] = {}
    all_ok = True
    for path in todo_files:
        try:
            diff = subprocess.check_output(
                ["git", "diff", "--cached", "-U0", "--", path],
                cwd=str(root), text=True, timeout=5,
                stderr=subprocess.DEVNULL,
            )
        except Exception:
            all_ok = False
            continue
        added_done: list[tuple[int, str]] = []
        removed_open: list[str] = []
        added_open: list[str] = []
        removed_done: list[tuple[int, str]] = []
        promoted_sections: list[str] = []
        flipped_here = False
        new_lineno = 0
        for line in diff.splitlines():
            hm = _HUNK_RE.match(line)
            if hm:
                # Settle the previous hunk before starting the next.
                hunk_promotions = _promoted_sections_for_hunk(root, path, added_done, removed_done)
                if hunk_promotions:
                    flipped_here = True
                    promoted_sections.extend(hunk_promotions)
                added_done.clear()
                removed_open.clear()
                added_open.clear()
                removed_done.clear()
                new_lineno = int(hm.group(1)) - 1
                continue
            if line.startswith("---") or line.startswith("+++"):
                continue
            if line.startswith("+"):
                new_lineno += 1
                if _IO_NEW_DONE_RE.match(line):
                    added_done.append((new_lineno, line))
                elif _IO_NEW_OPEN_RE.match(line):
                    added_open.append(line)
                continue
            if line.startswith("-"):
                if _IO_OLD_OPEN_RE.match(line):
                    removed_open.append(line)
                elif _IO_OLD_DONE_RE.match(line):
                    removed_done.append((max(new_lineno + 1, 1), "+" + line[1:]))
                continue
            if new_lineno:
                new_lineno += 1
        # Settle the final hunk.
        hunk_promotions = _promoted_sections_for_hunk(root, path, added_done, removed_done)
        if hunk_promotions:
            flipped_here = True
            promoted_sections.extend(hunk_promotions)
        if flipped_here:
            flips.append(path)
            sections_by_todo[path] = sorted(set(promoted_sections))
    return (flips, all_ok, sections_by_todo)


# ----------------------------------------------------------------------
# Evidence checks (post-signature; fail-CLOSED)
# ----------------------------------------------------------------------


def _build_receipt_check(root: Path) -> tuple[bool, str]:
    """Content-addressed build receipt check via scripts/overnight/receipts.py.
    (valid, reason); any import/exec failure reads as 'no receipt' so the
    caller falls back to the legacy TTL path (never a false pass)."""
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "overnight_receipts", str(root / "scripts/overnight/receipts.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod.check_build(root)
    except Exception as exc:  # noqa: BLE001
        return (False, f"receipt module unavailable ({exc})")


def _build_evidence(root: Path, staged_src: list[str]) -> tuple[bool, str]:
    """Codex C2 + design review: build/build.log shows BUILD OK AND
    its mtime is NEWER than every staged source file. Catches
    `build OK at minute 0; edit at minute 25; commit at minute 28`
    where mtime-only would falsely pass.

    Codex H2 round-2: build compiles WORKTREE content; commit ships
    INDEX content. If staged blob != worktree blob (user reverted
    worktree, built, then re-staged the change), mtime evidence is
    stale relative to the index. Block such desync.
    """
    log = root / "build" / "build.log"
    if not log.exists():
        return (False, "build/build.log missing")
    try:
        log_mtime = log.stat().st_mtime
    except Exception as exc:
        return (False, f"build/build.log stat failed: {exc}")
    try:
        with log.open("r", encoding="utf-8", errors="replace") as f:
            # tail -1
            last = ""
            for line in f:
                if line.strip():
                    last = line.strip()
    except Exception as exc:
        return (False, f"build/build.log read failed: {exc}")
    if "=== BUILD OK ===" not in last:
        return (False, f"build/build.log last line is {last!r} (expected '=== BUILD OK ===')")
    # Index/worktree desync check (Codex H2 round-2). `git diff
    # --name-only` (no --cached) reports paths whose worktree differs
    # from the index. If the build compiled the worktree but the index
    # has different content, the build is not evidence for the commit.
    # Applies to BOTH evidence paths: a receipt fingerprints the
    # WORKTREE; the commit ships the INDEX.
    desync = _index_worktree_desync(root, staged_src)
    if desync:
        return (False, f"staged content differs from worktree for "
                       f"{desync[:5]}; build compiled worktree, not index. "
                       f"Re-run build after staging or unstage divergent paths."
                       + (f" (+{len(desync)-5} more)" if len(desync) > 5 else ""))
    # Content-addressed receipt (2026-07-11): build.sh records
    # build/build-receipt.json binding the green build to the exact
    # build-input + toolchain fingerprints. A matching receipt is valid
    # REGARDLESS OF AGE -- a slow external review must not force a
    # rebuild over unchanged content. Fingerprint mismatch or a missing
    # receipt falls through to the legacy mtime+TTL path below.
    receipt_ok, receipt_why = _build_receipt_check(root)
    if receipt_ok:
        return (True, "")
    # Legacy fallback: 30-min wall-clock guardrail (mtime within window).
    age_s = time.time() - log_mtime
    if age_s > EVIDENCE_TTL_SECONDS:
        return (False, f"build/build.log mtime {age_s/60:.0f} min old "
                       f"(>{EVIDENCE_TTL_SECONDS/60:.0f} min legacy TTL) and "
                       f"no valid content receipt ({receipt_why}). "
                       f"Re-run bash scripts/build.sh.")
    # Every staged source file must have mtime <= log mtime.
    stale_after_build: list[str] = []
    for rel in staged_src:
        p = root / rel
        try:
            if p.stat().st_mtime > log_mtime + 1:  # 1s slop for FS rounding
                stale_after_build.append(rel)
        except FileNotFoundError:
            # Staged but deleted file: build.log can't have covered it
            # from disk, but git's index has the deletion. Don't block.
            continue
        except Exception:
            # Permission/IO: be conservative -- treat as evidence missing
            # for THIS file (post-signature fail-closed).
            stale_after_build.append(f"{rel} (stat error)")
    if stale_after_build:
        return (False, f"sources edited after build: {', '.join(stale_after_build[:5])}"
                       + (f" (+{len(stale_after_build)-5} more)" if len(stale_after_build) > 5 else ""))
    return (True, "")


def _review_evidence(root: Path, staged_src: list[str]) -> tuple[bool, str]:
    """Codex C2 fix: `received: true` within 30 min AND review's
    trigger_files is a superset of the staged source paths. Empty
    trigger_files = covers NOTHING (Codex's recommendation).
    """
    state_path = root / ".claude" / "state" / "last-codex-review.json"
    if not state_path.exists():
        return (False, ".claude/state/last-codex-review.json missing "
                       "-- no Codex review in this session?")
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
    except Exception as exc:
        return (False, f"last-codex-review.json malformed ({exc})")
    if not isinstance(state, dict):
        return (False, "last-codex-review.json is not a JSON object")
    if state.get("received") is not True:
        # P1.2 churn fix: `last-codex-review.json` holds only the SINGLE latest
        # review, so the next kind's trigger in a multi-kind broker round resets
        # received:false even though an EARLIER kind was genuinely received. That
        # produced the false blocks that forced the SKIP_REVIEW_HOOK bypasses.
        # Before blocking, consult the received-review ring buffer: if any prior
        # RECEIVED review blob-covers the EXACT current staged content, the work
        # is reviewed. Fail-safe: only a received review whose trigger_blobs match
        # the current staged blobs counts (content-bound, never a bare timestamp).
        cur_blobs = crc._staged_source_blobs(root, staged_src)
        if cur_blobs and _history_covers(root, staged_src, cur_blobs):
            return (True, "")
        trig = str(state.get("trigger", "(unknown)"))[:80]
        return (False, f"latest Codex review (trigger: {trig}) was not "
                       f"processed through Skill(superpowers:receiving-code-review), "
                       f"and no prior received review in the history covers this "
                       f"staged content. Run the receive skill, then retry the commit.")
    rts = state.get("received_timestamp_ns")
    if not isinstance(rts, int):
        return (False, "received_timestamp_ns missing or not an int")
    # NO wall-clock expiry (content-addressed receipts, 2026-07-11): the
    # blob-SHA binding below is the review's validity -- it holds exactly
    # while the staged content equals what the review covered, however long
    # ago that was, and breaks on the first post-review edit. A TTL on top
    # only forced re-reviews of unchanged content after slow review rounds.
    trigger_files = state.get("trigger_files") or []
    if not isinstance(trigger_files, list):
        return (False, "trigger_files is not a list")
    # Codex C2: empty trigger_files = covers nothing.
    if not trigger_files:
        return (False, "review's trigger_files is empty -- the trigger hook "
                       "captured no staged source at review time, so this review "
                       "cannot be bound to the staged tree. Re-stage source and "
                       "re-run the review, OR opt out via SKIP_REVIEW_HOOK with reason.")
    # Codex C1: content binding via blob SHA. Path coverage alone
    # lets post-review same-path edits sneak past. Compare current
    # staged blob SHAs against the trigger-time snapshot.
    trigger_blobs = state.get("trigger_blobs") or {}
    if not isinstance(trigger_blobs, dict) or not trigger_blobs:
        return (False, "review's trigger_blobs is empty or missing -- the trigger "
                       "hook did not capture per-path blob SHAs (likely an old "
                       "state file from before the C1 fix). Re-run the review.")
    current_blobs = crc._staged_source_blobs(root, staged_src)
    if not current_blobs:
        return (False, "could not read current staged blob SHAs via git ls-files")
    # P1.2: a record whose bound paths do not INTERSECT the staged source AT ALL
    # is a STALE record from a prior/unrelated review -- not partial coverage of
    # THIS commit. Reject it with a precise message (a bare "uncovered source"
    # reads as a coverage gap and historically sent runs down a re-stage rabbit
    # hole / a SKIP_REVIEW_HOOK bypass). Corroborate with the head SHA when the
    # review's HEAD has since moved.
    if not (set(trigger_blobs) & set(staged_src)):
        rec_head = str(state.get("head_sha") or "")
        cur_head = crc._head_sha(root)
        head_note = (f" -- review HEAD {rec_head[:12]} != current {cur_head[:12]}"
                     if rec_head and cur_head and rec_head != cur_head else "")
        return (False,
                f"last-codex-review.json is a STALE/unrelated record: its reviewed "
                f"files {sorted(trigger_blobs)[:3]} share NOTHING with this commit's "
                f"staged source {staged_src[:3]}{head_note}. Re-run the review against "
                f"the current staging, or SKIP_REVIEW_HOOK with a reason.")
    uncovered = [p for p in staged_src if p not in trigger_blobs]
    if uncovered:
        return (False, f"review covered paths {sorted(trigger_blobs.keys())[:3]}... but "
                       f"commit includes uncovered source: {uncovered[:5]}"
                       + (f" (+{len(uncovered)-5} more)" if len(uncovered) > 5 else ""))
    mismatched = [
        p for p in staged_src
        if not _source_binding_matches(trigger_blobs.get(p), current_blobs.get(p))
    ]
    if mismatched:
        # Fallback: check the ring-buffer history for any previously
        # received review whose trigger_blobs cover the *current* staged
        # tree. Prevents re-staging an identical content tree after a
        # stash-pop / re-add cycle from invalidating an already-received
        # review whose blob SHAs would still match.
        if _history_covers(root, staged_src, current_blobs):
            return (True, "")
        return (False, f"review covered older content of {mismatched[:5]}; "
                       f"current staged blob SHA differs (post-review edit). "
                       f"Re-run the review against the new staging."
                       + (f" (+{len(mismatched)-5} more)" if len(mismatched) > 5 else ""))
    return (True, "")


def _crashed_broker_legs(root: Path, todo: str, entry: dict) -> list[str]:
    """Kinds whose BROKER dispatch never completed (crashed / still running).

    Closes the dispatch-bound-receipt gap. The broker detaches each review into
    a transient unit and returns instantly, and its stamp is deliberately
    written at DISPATCH time -- `codex_review_completed._record_stamp` treats a broker
    dispatch as stampable because a broker leg IS a real review wrapper (the
    `is_broker` carve-out), unlike a generic `task --background`. That is right
    for ATTRIBUTION and wrong as COMPLETION proof: a leg that dies with
    `app-server exited unexpectedly, rc=1` leaves a stamp that satisfied the
    four-dispatch check while no review ever ran. Codex crashes are common
    (16 in one measured run), so this is not theoretical.

    The broker manifest is the authority on which dispatches were brokered and
    whether they finished, so no new state is introduced: a manifest entry for
    (todo, kind) means the dispatch WAS brokered, and `leg_summary` reports
    `complete` / `crashed` from the artifact itself.

    FAIL-OPEN in every direction it cannot prove a crash -- unreadable manifest,
    unimportable envelope module, no matching entry (a FOREGROUND
    `scripts/codex-dispatch.sh` dispatch is synchronous, so its return already
    proves completion and it has no manifest line). Only a positively-observed
    crashed-or-unfinished leg blocks.
    """
    try:
        mf = root / ".claude" / "overnight" / "reviews" / "manifest.jsonl"
        if not mf.exists():
            return []
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "_rev_env", root / "scripts" / "overnight" / "review-envelope.py")
        rev = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(rev)
        newest: dict[str, dict] = {}
        for line in mf.read_text(encoding="utf-8").splitlines():
            try:
                e = json.loads(line)
            except Exception:
                continue
            if e.get("todo") != todo:
                continue
            k = e.get("kind")
            if k in _DISPATCH_KINDS and (
                    k not in newest or (e.get("ts") or 0) >= (newest[k].get("ts") or 0)):
                newest[k] = e
        bad: list[str] = []
        for kind in _DISPATCH_KINDS:
            ts = entry.get(kind)
            if not isinstance(ts, int) or ts <= 0:
                continue          # already reported as missing
            e = newest.get(kind)
            if not e:
                continue          # foreground dispatch: nothing to prove here
            # Only judge a manifest entry from THIS dispatch onward; an older
            # brokered leg for a prior round must not condemn a fresh one.
            if (e.get("ts") or 0) * 1_000_000_000 < ts - 300_000_000_000:
                continue
            try:
                s = rev.leg_summary(e)
            except Exception:
                continue          # cannot read the artifact -> cannot prove a crash
            if s.get("crashed"):
                bad.append(f"{kind} (crashed, rc={s.get('rc')})")
            elif not s.get("complete"):
                bad.append(f"{kind} (no completion sentinel)")
        return bad
    except Exception:
        return []                 # a broken check must never block a commit


def _history_covers(root: Path, staged_src: list[str], current_blobs: dict) -> bool:
    """Return True if any received review in
    .claude/state/codex-review-history.jsonl covered every staged path
    with a blob SHA that matches the current staged blob. Bounded to the
    last 16 entries (file is a ring buffer)."""
    hp = root / ".claude" / "state" / "codex-review-history.jsonl"
    if not hp.exists():
        return False
    try:
        lines = hp.read_text(encoding="utf-8").splitlines()
    except Exception:
        return False
    # Walk newest-first.
    for line in reversed(lines):
        line = line.strip()
        if not line:
            continue
        try:
            entry = json.loads(line)
        except Exception:
            continue
        if entry.get("received") is not True:
            continue
        rts = entry.get("received_timestamp_ns")
        if not isinstance(rts, int):
            continue
        if (time.time_ns() - rts) / 1e9 > EVIDENCE_TTL_SECONDS:
            continue
        eblobs = entry.get("trigger_blobs") or {}
        if not isinstance(eblobs, dict):
            continue
        if all(_source_binding_matches(eblobs.get(p), current_blobs.get(p)) for p in staged_src):
            return True
    return False


def _source_binding_matches(recorded: object, current: object) -> bool:
    old = str(recorded or "")
    new = str(current or "")
    if old == new:
        return True
    old_parts = old.split(":", 1)
    new_parts = new.split(":", 1)
    old_blob = old_parts[1] if len(old_parts) == 2 and old_parts[0].isdigit() else old
    new_blob = new_parts[1] if len(new_parts) == 2 and new_parts[0].isdigit() else new
    if ":" not in old:
        return bool(old_blob and old_blob == new_blob)
    return False


# ----------------------------------------------------------------------
# §5 Four-dispatch policy: review-todo-section step-8 enforcement
# ----------------------------------------------------------------------


def _stamped_todos(root: Path, todo_files: list[str]) -> list[str]:
    """Return the subset of `todo_files` whose staged diff ADDS a
    NEW `**Verified:**` or `**Quality reviewed:**` stamp line. The
    four-dispatch check fires only when at least one such file is
    present in the staged diff.

    UPDATE-vs-FRESH discrimination: when a hunk contains BOTH an
    ADDED stamp line AND a REMOVED stamp line with the SAME label
    (`Verified` <-> `Verified`, `Quality reviewed` <-> `Quality
    reviewed`) the count is balanced and the file is treated as an
    in-place stamp UPDATE (e.g. a gap-audit follow-up that bumps the
    `7/7 items` count to `7/8 items, 2 deferred [/]` after filing a
    new follow-up item). Updates do not require fresh
    adversarial+consistency+perf dispatches. Only ADD lines without a
    matching removed counterpart count as fresh stamps.
    """
    out: list[str] = []
    for path in todo_files:
        try:
            diff = subprocess.check_output(
                ["git", "diff", "--cached", "-U0", "--", path],
                cwd=str(root), text=True, timeout=5,
                stderr=subprocess.DEVNULL,
            )
        except Exception:
            # Per-file diff failure: be conservative -- skip this
            # file. The signature-detection layer's "unknown" path
            # would have already routed this case via `todo_diff_ok`.
            continue
        added = {"Verified": 0, "Quality reviewed": 0}
        removed = {"Verified": 0, "Quality reviewed": 0}
        for line in diff.splitlines():
            m = _STAMP_ADDED_RE.match(line)
            if m:
                added[m.group(1)] += 1
                continue
            m = _STAMP_REMOVED_RE.match(line)
            if m:
                removed[m.group(1)] += 1
        # Fresh = added beyond what was removed. An update (added==removed
        # > 0) leaves a 0 net count and does not trigger the gate.
        net_fresh = sum(max(0, added[k] - removed[k]) for k in added)
        if net_fresh > 0:
            out.append(path)
    return out


def _git_is_ancestor(root: Path, sha: str, descendant_sha: str) -> bool:
    """True if `sha` is an ancestor of (or equal to) `descendant_sha`.

    Codex M2 (post-impl): cross-branch stamp reuse defense. When a
    dispatch is recorded against branch A's HEAD, switching to branch
    B and stamping there must not satisfy the gate unless A's HEAD is
    in B's ancestor chain (i.e., the work is part of B's history).

    Returns False if either SHA is missing/empty, if the git command
    fails (e.g. SHA no longer exists after a rebase), or if `git
    merge-base --is-ancestor` reports non-zero. Callers treat False
    as "binding does not hold" for the failing kind.
    """
    if not sha or not descendant_sha:
        return False
    if sha == descendant_sha:
        return True
    try:
        result = subprocess.run(
            ["git", "merge-base", "--is-ancestor", sha, descendant_sha],
            cwd=str(root), timeout=3,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
    except Exception:
        return False
    return result.returncode == 0


def _dispatch_content_fresh(root: Path, disp_head: str,
                            current_head: str) -> tuple[bool, str]:
    """Content-bound dispatch validity (replaces the wall-clock TTL when a
    dispatch head is recorded). A dispatch stays valid for as long as no
    SOURCE content changed since it ran:

      - disp_head == HEAD              -> fresh (nothing committed since)
      - disp_head ancestor of HEAD AND
        disp_head..HEAD touches no
        _DISPATCH_CONTENT_PATHS path   -> fresh (only TODO/doc/stamp commits
                                          landed, which the review's own fix
                                          loop produces)
      - otherwise                      -> stale/cross-branch, with the reason

    Age is irrelevant by design: a 3-hour-old dispatch over unchanged source
    is valid; a 5-minute-old one over changed source is not.
    """
    if not disp_head or not current_head:
        return (False, "no dispatch head recorded")
    if disp_head == current_head:
        return (True, "")
    if not _git_is_ancestor(root, disp_head, current_head):
        return (False, f"dispatch head {disp_head[:10]} not ancestor of "
                       f"HEAD {current_head[:10]} (cross-branch)")
    try:
        out = subprocess.check_output(
            ["git", "diff", "--name-only", disp_head, current_head],
            cwd=str(root), text=True, timeout=10,
            stderr=subprocess.DEVNULL)
    except Exception:
        return (False, f"could not diff {disp_head[:10]}..HEAD")
    drifted = [p for p in out.splitlines()
               if p.startswith(_DISPATCH_CONTENT_PATHS)]
    if drifted:
        return (False, f"source changed since dispatch: {drifted[:3]}"
                       + (f" (+{len(drifted)-3} more)" if len(drifted) > 3 else ""))
    return (True, "")


def _four_dispatch_evidence(
    root: Path, stamped_todos: list[str]
) -> tuple[bool, str]:
    """For each TODO whose staged diff adds a stamp, require all three
    dispatch entries (adversarial / consistency / perf) in
    last-review-stamps.json, each CONTENT-VALID: when the entry carries
    a `<kind>_head` field, the dispatch is valid while that head equals
    HEAD or is an ancestor with no source-path drift since
    (_dispatch_content_fresh; age-independent). Legacy entries without
    a head fall back to the FOUR_DISPATCH_TTL_SECONDS wall clock.

    Returns (ok, error_message). Empty `stamped_todos` -> (True, "")
    (the four-dispatch check does not fire on implementation commits;
    it only gates stamp-add commits, i.e. review / verify section
    completions).

    Missing state file or non-dict shape -> fail with an actionable
    message naming the missing dispatches for ALL stamped todos
    (collapsed to one message). Per-todo missing dispatches are
    enumerated in the message so the agent knows exactly what to
    re-run.

    Backwards compatibility: state entries written before the M2 fix
    have `<kind>` (ts_ns) but no `<kind>_head` field. These pass the
    ancestry check (TTL-only) and emit a one-shot stderr WARN so
    legacy stamps remain usable during the transition window.
    """
    if not stamped_todos:
        return (True, "")
    stamps_path = root / ".claude" / "state" / "last-review-stamps.json"
    if not stamps_path.exists():
        joined = ", ".join(stamped_todos[:3])
        more = (
            f" (+{len(stamped_todos)-3} more)"
            if len(stamped_todos) > 3 else ""
        )
        return (False, (
            f".claude/state/last-review-stamps.json missing -- no "
            f"review-todo-section step-8 dispatches recorded yet for "
            f"{joined}{more}. Run `codex-adversarial-review-section`, "
            f"`codex-consistency-audit`, AND `codex-perf-review` (each "
            f"with the [review-kind: <kind>] marker AND the TODO path "
            f"in the prompt) before stamping."
        ))
    try:
        state = json.loads(stamps_path.read_text(encoding="utf-8"))
    except Exception as exc:
        return (False, f"last-review-stamps.json malformed ({exc})")
    if not isinstance(state, dict):
        return (False, "last-review-stamps.json is not a JSON object")

    now_ns = time.time_ns()
    cutoff_ns = now_ns - FOUR_DISPATCH_TTL_SECONDS * 1_000_000_000

    # Resolve current HEAD once for the ancestry check (Codex M2).
    current_head = ""
    try:
        current_head = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=str(root),
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        pass

    # Content-bound freshness cache by (dispatch_head, current_head) pair
    # (perf lineage: Codex post-commit M1(b) ancestry cache). Distinct
    # dispatch heads are diffed once per commit, not once per kind*todo.
    freshness_cache: dict[tuple[str, str], tuple[bool, str]] = {}

    legacy_warned = False
    issues: list[str] = []
    for todo in stamped_todos:
        entry = state.get(todo)
        if not isinstance(entry, dict):
            issues.append(
                f"{todo}: no dispatch entries recorded -- run all three "
                f"dispatches with the TODO path in the prompt"
            )
            continue
        missing: list[str] = []
        stale: list[str] = []
        cross_branch: list[str] = []
        for kind in _DISPATCH_KINDS:
            ts = entry.get(kind)
            if not isinstance(ts, int) or ts <= 0:
                missing.append(kind)
                continue
            disp_head = entry.get(f"{kind}_head")
            if disp_head and current_head:
                # Content-bound validity: valid while no source changed
                # since the dispatch's HEAD, regardless of wall-clock age.
                cache_key = (disp_head, current_head)
                if cache_key not in freshness_cache:
                    freshness_cache[cache_key] = _dispatch_content_fresh(
                        root, disp_head, current_head
                    )
                fresh, why = freshness_cache[cache_key]
                if not fresh:
                    if "cross-branch" in why:
                        cross_branch.append(f"{kind} ({why})")
                    else:
                        stale.append(f"{kind} ({why})")
                    continue
            else:
                # Legacy entry without a head binding: wall-clock TTL is the
                # only available freshness signal.
                if ts < cutoff_ns:
                    age_min = (now_ns - ts) / 1e9 / 60
                    stale.append(f"{kind} ({age_min:.0f} min old, legacy "
                                 f"no-head entry)")
                    continue
                if current_head and not legacy_warned:
                    sys.stderr.write(
                        f"[section-commit-gate] WARN: {todo} {kind} dispatch "
                        f"entry predates the head-binding fix (no `{kind}_head` "
                        f"field). Allowing on TTL alone for this commit; future "
                        f"dispatches will record the binding.\n"
                    )
                    legacy_warned = True
        crashed = _crashed_broker_legs(root, todo, entry)
        if missing or stale or cross_branch or crashed:
            parts: list[str] = []
            if missing:
                parts.append(f"missing: {', '.join(missing)}")
            if stale:
                parts.append(f"stale (source drift or legacy TTL): "
                             f"{', '.join(stale)}")
            if cross_branch:
                parts.append(
                    f"cross-branch (not ancestor of HEAD): {', '.join(cross_branch)}"
                )
            if crashed:
                parts.append(
                    f"dispatched but never COMPLETED: {', '.join(crashed)} "
                    f"-- re-dispatch those legs (review-envelope.py "
                    f"`needs_redispatch` names them); a crashed leg's "
                    f"dispatch-time stamp is attribution, not review proof"
                )
            issues.append(f"{todo}: {'; '.join(parts)}")

    if issues:
        joined = " | ".join(issues[:5])
        more = f" (+{len(issues)-5} more)" if len(issues) > 5 else ""
        return (False, (
            f"review-todo-section step-8 four-dispatch policy violated. "
            f"Stamps added without all three dispatches content-valid "
            f"(recorded, head-bound, no source drift since): {joined}{more}. "
            f"Re-run the missing dispatches; each prompt MUST include "
            f"`[review-kind: adversarial|consistency|perf]` and the "
            f"TODO path so the §3 PostToolUse hook can attribute it."
        ))
    return (True, "")


def _staged_diff_text(root: Path, paths: list[str]) -> str | None:
    """Return the concatenated `git diff --cached` body for the given
    staged source paths. Used by the re-adversarial trigger detector
    to scan only ADDED lines (the prefix `+` filter happens at the
    regex layer; see _re_adversarial_trigger_check).

    Codex re-adversarial M2 fix (2026-04-28): returns None on git
    failure (timeout, missing binary, etc.) so the caller can
    distinguish 'no diff body' from 'could not inspect diff'. The
    earlier fail-open empty-string return let a transient git
    failure suppress trigger detection."""
    if not paths:
        return ""
    try:
        out = subprocess.check_output(
            ["git", "diff", "--cached", "--unified=0", "--"] + paths,
            cwd=str(root), text=True, timeout=10,
            stderr=subprocess.DEVNULL,
        )
        return out or ""
    except Exception:
        return None


def _staged_loc_delta(root: Path, paths: list[str]) -> int | None:
    """Total added+removed LOC across staged C/H/asm files. Returns
    None on git failure (Codex re-adversarial M2 fix); 0 when no
    paths provided OR no LOC change observed."""
    if not paths:
        return 0
    try:
        out = subprocess.check_output(
            ["git", "diff", "--cached", "--numstat", "--"] + paths,
            cwd=str(root), text=True, timeout=10,
            stderr=subprocess.DEVNULL,
        )
    except Exception:
        return None
    total = 0
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        try:
            total += int(parts[0]) + int(parts[1])
        except ValueError:
            # Binary files report `-\t-`; skip.
            continue
    return total


# A skill entry older than this is treated as abandoned, not in flight.
# 6h is deliberately generous: the longest real review-todo-section observed
# (TODO-21 section 19, 2026-07-28) ran about one hour end to end, so this only
# fires on an entry no live skill is still writing to. Override for a genuinely
# longer pass with SECTION_SKILL_STALE_HOURS.
_SKILL_STALE_HOURS_DEFAULT = 6


def _skill_entry_stale(entry: dict) -> bool:
    """True when a skill-progress entry is a leftover, not an in-flight skill.

    WHY THIS EXISTS. skill-progress.json is append-only across sessions and is
    never pruned: measured 2026-07-28 it held 346 entries from 53 distinct
    session ids, 343 of them already flagged compaction_orphaned, the oldest
    about 90 days. The orphan flag covers entries a PreCompact saw; it does NOT
    cover an entry whose session simply ended -- an unattended run that finishes
    or is disarmed leaves its live entry behind, still unflagged.

    That is not hypothetical. On 2026-07-28 the disarmed overnight run left
    `review-todo-section` pointing at TODO-21 section 19 (session
    e711ac7b-..., started 09:04). Hours later an unrelated interactive commit
    touching only boot_timing.c and dpc.c was refused as a "fix-loop commit"
    needing re-adversarial evidence for section 19 -- the gate had adopted
    another session's skill as the committer's own. The gate never compared
    session ids; it still cannot in --git-hook-mode, which is the load-bearing
    path and receives no stdin payload. Age is the signal that works in both
    modes.

    Fails OPEN on a missing or unparseable timestamp: an entry that cannot be
    dated is treated as live, so a real in-flight skill is never dropped by a
    clock problem. This check can only ever RELAX the gate, never tighten it.
    """
    try:
        started = entry.get("started_ts")
        if not isinstance(started, int) or started <= 0:
            return False                    # undatable -> assume live
        hours = float(os.environ.get("SECTION_SKILL_STALE_HOURS",
                                     _SKILL_STALE_HOURS_DEFAULT))
        if hours <= 0:
            return False                    # explicitly disabled
        age_ns = time.time_ns() - started
        return age_ns > int(hours * 3600 * 1e9)
    except Exception:
        return False                        # any doubt -> assume live


def _active_section_todo(root: Path) -> str:
    """The TODO path of the in-flight implement/review-todo-section skill, or "".

    Reads skill-progress.json, preferring the structured `todo_path` recorded by
    skill_step_observer at skill-start (a single authoritative regex run) over a
    loose parse of `args`. Checks review-todo-section first (the section-ship
    context), then implement-todo-section (a fix-loop commit before review)."""
    state_path = root / ".claude" / "state" / "skill-progress.json"
    if not state_path.exists():
        return ""
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
    except Exception:
        return ""
    if not isinstance(state, dict):
        return ""
    for skill in ("review-todo-section", "implement-todo-section"):
        entry = state.get(skill)
        if not isinstance(entry, dict) or entry.get("compaction_orphaned") is True:
            continue
        if _skill_entry_stale(entry):
            continue
        todo_path = entry.get("todo_path")
        if isinstance(todo_path, str) and todo_path.endswith(".md") \
           and "todo/" in todo_path.replace("\\", "/"):
            return todo_path
        args = entry.get("args", "")
        if isinstance(args, str) and args:
            m = re.search(r"todo/[\w./-]+TODO-\d[\w./-]*\.md", args)
            if m:
                return m.group(0)
    return ""


def _attribute_review_todo(root: Path, todo_files: list[str]) -> str:
    """Pick the TODO path to look up in last-review-stamps.json for the
    re-adversarial check.

    F1: prefer the ACTIVE section's TODO (the implement/review-todo-section
    context) over "first staged .md". When a section-ship commit stages the
    section's own TODO alongside stale reciprocal-XREF edits in OTHER TODOs
    (TODO-22 + TODO-12/TODO-07 on the 2026-07-13 run), first-staged picked the
    wrong file, blocked the commit, and forced a `git restore --staged` dance.
    The active-section context is the authoritative owner. Falls back to the
    first staged TODO .md only when there is no active section context.
    """
    active = _active_section_todo(root)
    if active:
        return active
    for tf in todo_files:
        if tf and tf.endswith(".md") and "todo/" in tf.replace("\\", "/"):
            return tf
    return ""


def _re_adversarial_trigger_check(
    root: Path, staged_src: list[str], todo_files: list[str]
) -> tuple[bool, str]:
    """TODO-08 §17 re-adversarial trigger gate.

    Returns (ok, error_message). When the staged C/H diff matches any
    of the step-13.5 triggers AND last-review-stamps.json lacks a
    `re-adversarial` entry for the relevant TODO within the TTL, BLOCK.

    Triggers (per Codex design review 2026-04-28 M3):
      - locking: spinlock_t / atomic_t / atomic\\d+_t / mutex_t
      - faultable: boot_halt / panic / KeBugCheckEx / fault_handler /
        exception_*
      - lifecycle: \\w*_(alloc|free|refcount)
      - LOC: total added+removed > 50

    State-machine and >3-functions are NOT mechanically detected --
    documented as manual-only gaps.

    Honors the existing SKIP_REVIEW_HOOK opt-out (caller already
    handles it; this function is reached only in the fail-closed
    paths).
    """
    # TODO-08 §22 #2 bootstrap-mode: when this hook's own file is in
    # the staged diff, downgrade BLOCK to WARN. Section_commit_gate
    # itself is the regression target since shipping new triggers in
    # _RE_ADV_TRIGGER_LOCKING / _RE_ADV_TRIGGER_FAULTABLE / etc. lands
    # in this very file.
    try:
        from _bootstrap_mode import is_bootstrap_commit
        if is_bootstrap_commit(__file__, root):
            sys.stderr.write(
                "[section-commit-gate] WARN (bootstrap-mode) -- staged "
                "diff includes this hook's own file; re-adversarial "
                "trigger gate downgraded from BLOCK to WARN for the "
                "commit that ships it.\n"
            )
            return (True, "")
    except Exception:
        pass

    # Regex scan restricted to C/H (locking/faultable/lifecycle vocab
    # is a C/C++ idiom). LOC trigger covers C/H + asm so an asm-only
    # rewrite of an interrupt entry / IST stub still trips the gate
    # (Codex adversarial M1 fix 2026-04-28).
    c_h_paths = [p for p in staged_src if p.endswith((".c", ".h"))]
    loc_paths = [p for p in staged_src if p.endswith((".c", ".h", ".asm", ".S"))]
    if not c_h_paths and not loc_paths:
        return (True, "")

    # ADDED-line filter: the diff body has unified-0 hunks; ADDED lines
    # start with `+` (and are not `+++` headers). We scan only those
    # lines for trigger signatures.
    # Codex re-adversarial M2 fix: helpers return None on git failure.
    # When inspection fails on a non-empty staged source set, we cannot
    # prove triggers did NOT fire -- emit a stderr WARN naming the
    # opacity and continue with whatever evidence we have. The
    # existing fail-closed git-hook mode in _evaluate catches genuine
    # git outage at a broader layer.
    # TODO-08 §17 deferred-XREF H1 fix: fail-closed when git inspection
    # is opaque on a non-empty staged source set. WARN+empty-diff used to
    # let a transient git failure suppress trigger detection. Now: when
    # paths exist but the helper returns None, return (False, error) so
    # the commit BLOCKs with an actionable message. Empty staged_src
    # remains (True, "") -- nothing to inspect.
    diff_text = _staged_diff_text(root, c_h_paths) if c_h_paths else ""
    if diff_text is None:
        return (False, (
            f"git inspection of staged C/H diff failed (timeout / "
            f"missing binary / transient error); cannot prove step-13.5 "
            f"triggers did NOT fire. Re-run the commit; if the failure "
            f"persists, set SKIP_REVIEW_HOOK with a reason after manual "
            f"step-13.5 verification."
        ))
    added_lines: list[str] = []
    for line in diff_text.splitlines():
        if line.startswith("+++"):
            continue
        if line.startswith("+"):
            added_lines.append(line[1:])
    added = "\n".join(added_lines)

    triggers_fired: list[str] = []
    if _RE_ADV_TRIGGER_LOCKING.search(added):
        triggers_fired.append("locking")
    if _RE_ADV_TRIGGER_FAULTABLE.search(added):
        triggers_fired.append("faultable region")
    if _RE_ADV_TRIGGER_LIFECYCLE.search(added):
        triggers_fired.append("lifecycle")
    loc = _staged_loc_delta(root, loc_paths)
    if loc is None and loc_paths:
        # Same fail-closed treatment for numstat opacity (H1 fix).
        return (False, (
            f"git numstat inspection failed on staged C/H/asm paths; "
            f"cannot evaluate >50 LOC step-13.5 trigger. Re-run the "
            f"commit; if persistent, set SKIP_REVIEW_HOOK with reason."
        ))
    if loc is not None and loc > _RE_ADV_LOC_THRESHOLD:
        triggers_fired.append(f">{_RE_ADV_LOC_THRESHOLD} LOC C/H/asm")

    if not triggers_fired:
        return (True, "")

    # Triggers fired; require a re-adversarial entry in
    # last-review-stamps.json[<todo>] within TTL.
    todo = _attribute_review_todo(root, todo_files)
    if not todo:
        # No attribution path -- emit a gentle message but don't
        # BLOCK. A pure-source fix commit with no active review skill
        # and no staged TODO file has nothing for the gate to look
        # up; the responsibility falls back to the agent's manual
        # judgment per step 13.5 prose.
        sys.stderr.write(
            "[section-commit-gate] WARN -- staged diff matches step-13.5 "
            f"trigger(s) {triggers_fired} but no TODO file is staged AND "
            "no active review-todo-section skill is in flight. The "
            "re-adversarial gate cannot attribute the commit; doctrine "
            "(feedback_re_adversarial_small_fix) requires you to dispatch "
            "re-adversarial manually if this is part of a fix loop.\n"
        )
        return (True, "")

    stamps_path = root / ".claude" / "state" / "last-review-stamps.json"
    re_adv_ts = 0
    re_adv_head = ""
    if stamps_path.exists():
        try:
            state = json.loads(stamps_path.read_text(encoding="utf-8"))
            if isinstance(state, dict):
                entry = state.get(todo)
                if isinstance(entry, dict):
                    ts = entry.get("re-adversarial")
                    if isinstance(ts, int) and ts > 0:
                        re_adv_ts = ts
                    rh = entry.get("re-adversarial_head")
                    if isinstance(rh, str):
                        re_adv_head = rh
        except Exception:
            pass

    now_ns = time.time_ns()
    cutoff_ns = now_ns - RE_ADV_TRIGGER_TTL_SECONDS * 1_000_000_000

    # Content-bound validity (2026-07-11; supersedes the pure-TTL check).
    # When the re-adv entry carries a head SHA (Codex adversarial H1,
    # 2026-04-28), the dispatch is valid while that head equals HEAD or is
    # an ancestor with no source drift since -- wall-clock age irrelevant.
    # Legacy entries with no head fall back to the TTL.
    if re_adv_ts > 0 and re_adv_head:
        current_head = ""
        try:
            current_head = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=str(root),
                text=True, timeout=2, stderr=subprocess.DEVNULL,
            ).strip()
        except Exception:
            pass
        fresh, why = _dispatch_content_fresh(root, re_adv_head, current_head)
        if fresh:
            return (True, "")
        return (False, (
            f"step-13.5 trigger(s) fired ({', '.join(triggers_fired)}) "
            f"on {todo}; the recorded `re-adversarial` dispatch is not "
            f"content-valid ({why}). Re-run the re-adversarial Codex "
            f"with [review-kind: re-adversarial] in the prompt."
        ))

    if re_adv_ts >= cutoff_ns and re_adv_ts > 0:
        # Legacy no-head entry inside the TTL window.
        return (True, "")

    if re_adv_ts > 0:
        age_min = (now_ns - re_adv_ts) / 1e9 / 60
        return (False, (
            f"step-13.5 trigger(s) fired ({', '.join(triggers_fired)}) on "
            f"{todo} but the recorded `re-adversarial` dispatch is stale "
            f"(legacy no-head entry, {age_min:.0f} min old; TTL "
            f"{RE_ADV_TRIGGER_TTL_SECONDS // 60} min). Re-run the "
            f"re-adversarial Codex with [review-kind: re-adversarial] "
            f"in the prompt before committing."
        ))
    return (False, (
        f"step-13.5 trigger(s) fired ({', '.join(triggers_fired)}) on "
        f"{todo} but no `re-adversarial` dispatch is recorded within "
        f"the last {RE_ADV_TRIGGER_TTL_SECONDS // 60} min. Doctrine: "
        f"feedback_re_adversarial_small_fix -- 'small fix' is NOT a "
        f"reason to skip step 13.5. Dispatch re-adversarial Codex with "
        f"[review-kind: re-adversarial] + the TODO path in the prompt, "
        f"then retry the commit. Opt-out (legitimate skip): "
        f"SKIP_REVIEW_HOOK=1 SKIP_REVIEW_HOOK_REASON=\"<text>\"."
    ))


# ----------------------------------------------------------------------
# SKIP path (Codex M1)
# ----------------------------------------------------------------------


def _skip_log_path(root: Path) -> Path:
    return root / ".claude" / "state" / "skip-log.jsonl"


def _skip_record_append(record: dict, path: Path) -> bool:
    """Append a single-line JSON record to skip-log.jsonl atomically.
    Uses O_APPEND + a single os.write() under PIPE_BUF (4096) so
    parallel sessions don't interleave -- POSIX guarantees atomicity
    for writes < PIPE_BUF on O_APPEND files. Codex M1.
    """
    line = json.dumps(record, ensure_ascii=True) + "\n"
    data = line.encode("utf-8")
    if len(data) >= 4096:
        # Truncate reason if the record exceeds PIPE_BUF -- atomic
        # append is the load-bearing property; an oversize line could
        # interleave. Trim the reason field and retry once.
        rec_copy = dict(record)
        rec_copy["reason"] = (rec_copy.get("reason", "")[:500] + "...[truncated for atomic append]")
        line = json.dumps(rec_copy, ensure_ascii=True) + "\n"
        data = line.encode("utf-8")
        if len(data) >= 4096:
            return False
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
        try:
            os.write(fd, data)
        finally:
            os.close(fd)
        return True
    except Exception:
        return False


def _reset_review_state(root: Path) -> None:
    """After a successful SKIP, write last-codex-review.json with
    received: false so the NEXT commit cannot reuse this review's
    `received: true` to satisfy its gate. Codex M1: skip is a state
    transition, not just an audit append.
    """
    state_path = root / ".claude" / "state" / "last-codex-review.json"
    if not state_path.exists():
        return  # nothing to invalidate
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
    except Exception:
        return  # malformed; leave alone -- next gate will fail closed anyway
    if not isinstance(state, dict):
        return
    if state.get("received") is not True:
        return  # already not-received, nothing to do
    state["received"] = False
    state["skipped_section_commit"] = True
    # Use the §3 hook's atomic writer so per-process tmp paths avoid
    # the race documented in codex_review_completed._write_atomic.
    crc._write_atomic(state_path, state)


def _emit_block(reason_label: str, details: list[str], head: str = "") -> None:
    sys.stderr.write(
        f"[section-commit-gate] BLOCK -- {reason_label}.\n"
    )
    for d in details:
        sys.stderr.write(f"[section-commit-gate]   {d}\n")
    sys.stderr.write(
        "[section-commit-gate] policy: implement-todo-section steps 13-18 "
        "produce the evidence this gate checks. Skipping any of them = "
        "blocked commit, not reminder-then-proceed.\n"
        "[section-commit-gate] opt-out: set BOTH "
        "SKIP_REVIEW_HOOK=1 AND "
        "SKIP_REVIEW_HOOK_REASON=\"<plain-language reason, "
        f">= {SKIP_REASON_MIN_LEN} chars>\" on the same call. The reason "
        "is logged to .claude/state/skip-log.jsonl AND last-codex-review.json "
        "is reset so the skip cannot be reused for the next commit.\n"
        "[section-commit-gate] doctrine: CLAUDE.md Mandatory Skill "
        "Triggers + memory feedback_never_skip_review / "
        "feedback_no_corner_cutting.\n"
    )
    if head:
        sys.stderr.write(f"[section-commit-gate] HEAD: {head[:12]}\n")


# ----------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------


def _repo_root() -> Path | None:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return None
    return Path(out) if out else None


def _scan_inline_env_prefix(cmd: str) -> dict[str, str]:
    """One-line delegation to the shared SKIP-env scanner (TODO-08
    section-23 unification). Preserves the original API shape -- only
    `SKIP_REVIEW_HOOK*` keys returned, no environ fallback -- so
    existing call sites in this file work unchanged. New consumers
    should call `_skip_env.read_skip_envs()` directly with their
    explicit key list.

    The previous in-file implementation has moved to
    `.claude/hooks/_skip_env.py` along with the wrapper-walk logic;
    that helper is the single source of truth across all 7 PreToolUse
    gates that honor a `SKIP_*` opt-out.
    """
    # Lazy import to keep this file's load cost the same as before.
    if str(_HOOK_DIR) not in sys.path:
        sys.path.insert(0, str(_HOOK_DIR))
    import _skip_env as _se  # noqa: E402
    return _se.read_skip_envs(
        cmd,
        keys=("SKIP_REVIEW_HOOK", "SKIP_REVIEW_HOOK_REASON"),
        fallback_to_environ=False,
    )


def _is_skip_requested(cmd: str = "") -> tuple[bool, str | None, str | None]:
    """Returns (skip_requested, reason, error). If SKIP_REVIEW_HOOK=1
    is set BUT SKIP_REVIEW_HOOK_REASON is missing or too short, returns
    skip_requested=True with error explaining what's missing -- the
    caller blocks with a usage envelope. Both env vars together = real
    skip path.

    Reads BOTH inline cmd env-prefix AND `os.environ` via the shared
    `_skip_env` helper (TODO-08 §23 unification). Inline wins on key
    collision per the helper's documented merge semantics -- so a same-
    call `SKIP_REVIEW_HOOK=0 git commit ...` overrides a stale ambient
    `SKIP_REVIEW_HOOK=1` (Codex review-impl H1 fix 2026-04-29: the old
    "env first, inline only as fallback when env is absent" behavior
    let stale ambient envs override same-call intent).
    """
    if str(_HOOK_DIR) not in sys.path:
        sys.path.insert(0, str(_HOOK_DIR))
    import _skip_env as _se  # noqa: E402
    merged = _se.read_skip_envs(
        cmd,
        keys=("SKIP_REVIEW_HOOK", "SKIP_REVIEW_HOOK_REASON"),
        fallback_to_environ=True,
    )
    flag = merged.get("SKIP_REVIEW_HOOK", "")
    reason = merged.get("SKIP_REVIEW_HOOK_REASON", "").strip()
    if flag != "1":
        return (False, None, None)
    if not reason:
        return (True, None, "SKIP_REVIEW_HOOK=1 set but SKIP_REVIEW_HOOK_REASON is empty")
    if len(reason) < SKIP_REASON_MIN_LEN:
        return (True, None, f"SKIP_REVIEW_HOOK_REASON too short "
                            f"({len(reason)} < {SKIP_REASON_MIN_LEN}); "
                            f"explain why the gate is being bypassed")
    return (True, reason, None)


def _harness_command_is_git_commit() -> tuple[bool, bool, str]:
    """Read the Claude Code hook payload from stdin and return
    (is_git_commit, has_no_verify, command_string). Fail-OPEN on
    malformed input (returns False) -- pre-signature errors should
    not block tool calls per Codex H2.

    Round-7 perf fix: cheap literal screen runs BEFORE the expensive
    shlex+wrapper-walk parse. Non-git commands (ls, cat, echo, the
    overwhelming majority of Bash invocations) return immediately
    without paying the parse cost or importing crc.
    """
    try:
        payload = json.load(sys.stdin)
    except Exception:
        return (False, False, "")
    if payload.get("tool_name") != "Bash":
        return (False, False, "")
    cmd = (payload.get("tool_input") or {}).get("command", "")
    if not isinstance(cmd, str):
        return (False, False, "")
    if not _looks_like_git_commit_screen(cmd):
        return (False, False, cmd)
    # Screen passed; lazy-import crc and run the real parse.
    _ensure_crc()
    is_commit, no_verify = _bash_is_git_commit(cmd)
    return (is_commit, no_verify, cmd)


def _git_hook_mode_main(root: Path) -> int:
    """Called from .githooks/pre-commit (which runs at commit time
    against the FINAL staging). Trigger condition is implicit -- if
    we got here, git is about to commit -- so we skip the harness
    `git commit` detection and go straight to signature + evidence.
    """
    return _evaluate(root, mode="git-hook")


def _harness_main() -> int:
    is_commit, has_no_verify, cmd = _harness_command_is_git_commit()
    if not is_commit:
        return 0
    # Codex H1: --no-verify skips .githooks/pre-commit entirely, so
    # the harness layer is the ONLY place a hard block can fire on
    # that bypass shape. Block unconditionally; the user's policy
    # already forbids --no-verify (CLAUDE.md "Never skip hooks").
    if has_no_verify:
        sys.stderr.write(
            "[section-commit-gate] BLOCK -- `git commit --no-verify` (or `-n`) "
            "skips .githooks/pre-commit, which is the load-bearing layer of "
            "the section-commit gate. The harness layer cannot replace it.\n"
            "[section-commit-gate]   Policy: never skip hooks unless the user "
            "explicitly asked (CLAUDE.md commit-safety protocol).\n"
            "[section-commit-gate]   If a hook is genuinely failing, fix the "
            "underlying issue rather than bypassing.\n"
            "[section-commit-gate]   Opt-out for the section-commit gate: "
            "SKIP_REVIEW_HOOK=1 + SKIP_REVIEW_HOOK_REASON=... (without "
            "--no-verify; the .githooks/pre-commit layer recognizes it).\n"
        )
        return 2
    root = _repo_root()
    if root is None:
        # Outside a repo (shouldn't happen for a git-commit Bash);
        # fail open per H2 pre-signature semantics.
        return 0
    return _evaluate(root, mode="harness", cmd=cmd)


def _evaluate(root: Path, mode: str, cmd: str = "") -> int:
    """Shared signature + evidence evaluation. Pre-signature errors
    fail OPEN; post-signature errors and missing evidence fail CLOSED.

    `cmd` is the harness's tool_input.command (when invoked from the
    Bash PreToolUse path); passed through to `_is_skip_requested` so
    inline `SKIP_REVIEW_HOOK=1 git commit ...` is recognized when the
    harness doesn't propagate shell env to the hook process. Empty
    string in --git-hook-mode (where git itself propagates env, so
    the canonical os.environ path works).
    """
    # Ensure crc is loaded before any signature/evidence work that
    # uses crc.X helpers. Harness path already loaded it during screen
    # pass; --git-hook-mode path enters here directly.
    _ensure_crc()
    # SKIP path: handle BEFORE signature detection so a clearly opted-
    # out commit doesn't waste cycles on diff/regex work AND so an
    # incomplete opt-out (missing reason) is reported with a usage
    # envelope.
    skip_req, skip_reason, skip_err = _is_skip_requested(cmd)
    if skip_req and skip_err:
        sys.stderr.write(
            f"[section-commit-gate] BLOCK -- opt-out malformed: {skip_err}.\n"
            f"[section-commit-gate]   Required: SKIP_REVIEW_HOOK=1 AND "
            f"SKIP_REVIEW_HOOK_REASON=\"<text >= {SKIP_REASON_MIN_LEN} chars>\".\n"
        )
        return 2

    try:
        sig_state, staged_src, flipped, flipped_sections = _detect_signature(root)
    except Exception:
        # Pre-signature unhandled exception: harness mode fails open;
        # git-hook-mode fails closed on non-empty index (Codex H2).
        if mode == "git-hook" and _index_is_nonempty(root):
            sys.stderr.write(
                "[section-commit-gate] BLOCK -- signature detection raised an "
                "unhandled exception while inspecting a non-empty index. The "
                "git-hook layer is the load-bearing enforcement point; it "
                "cannot fail open. Retry the commit; if the failure persists, "
                "investigate the gate.\n"
            )
            return 2
        return 0
    if sig_state == "not_section":
        # TODO-08 §17 (Codex design H2): pure-source fix-loop commits
        # carry no IO flip and no stamp, so the section-commit signature
        # is "not_section" -- but if review-todo-section is in flight
        # (skill-progress.json has an active entry), step 13.5 still
        # applies. Fire the re-adversarial trigger check using the
        # active skill's TODO path for attribution. SKIP_REVIEW_HOOK
        # honored.
        skip_req2, skip_reason2, skip_err2 = _is_skip_requested(cmd)
        if skip_req2 and not skip_err2 and skip_reason2:
            return 0
        all_staged_todos2 = _staged_todo_files(root)
        re_adv_ok, re_adv_err = _re_adversarial_trigger_check(
            root, staged_src, all_staged_todos2
        )
        if re_adv_ok:
            return 0
        head = ""
        try:
            head = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=str(root),
                text=True, timeout=2, stderr=subprocess.DEVNULL,
            ).strip()
        except Exception:
            pass
        _emit_block(
            "fix-loop commit detected (active review-todo-section + "
            "step-13.5 trigger in staged C/H diff) but re-adversarial "
            "evidence is missing",
            [f"re-adversarial evidence: {re_adv_err}"],
            head=head,
        )
        return 2
    if sig_state == "unknown":
        # Codex H2: in --git-hook-mode (load-bearing), unknown +
        # non-empty index = block. In harness mode (convenience),
        # unknown = fail open per the pre-signature semantics.
        if mode == "git-hook":
            sys.stderr.write(
                "[section-commit-gate] BLOCK -- could not fully inspect the "
                "staged TODO diff (per-file `git diff` failed/timed out) but "
                "the index is non-empty. The git-hook layer cannot fail open "
                "on a real commit. Retry the commit (transient git issue); "
                "if persistent, investigate.\n"
            )
            return 2
        return 0

    if sig_state == "stamp_only":
        # Codex M1 (post-impl): stamp-only commits (the user's
        # `stamp: TODO-XX §N` workflow) trigger ONLY the four-dispatch
        # check. No source was reviewed, so build/review evidence
        # is not applicable. SKIP env vars still bypass.
        if skip_req and skip_reason:
            head = ""
            try:
                head = subprocess.check_output(
                    ["git", "rev-parse", "HEAD"], cwd=str(root),
                    text=True, timeout=2, stderr=subprocess.DEVNULL,
                ).strip()
            except Exception:
                pass
            record = {
                "timestamp_ns": time.time_ns(),
                "mode": mode,
                "head_sha": head,
                "staged_sources": staged_src,
                "stamped_todos": flipped,  # repurposed slot: stamped TODOs
                "kind": "stamp_only",
                "reason": skip_reason,
            }
            if not _skip_record_append(record, _skip_log_path(root)):
                sys.stderr.write(
                    "[section-commit-gate] BLOCK -- SKIP audit write to "
                    ".claude/state/skip-log.jsonl FAILED. The opt-out path "
                    "requires a durable paper trail; refusing the commit.\n"
                )
                return 2
            # B3 (Canary #2, 2026-07-14): do NOT reset last-codex-review.json
            # `received` for a STAMP-ONLY SKIP. A stamp-only commit touches only
            # TODO markdown (adds a `**Verified:**` stamp) and does NOT change the
            # source the code review passed -- so the code review's `received: true`
            # still legitimately stands, and the run needs it to persist for the
            # immediately-following full `rollover` and the receiving-review gate.
            # The reset here was redundant anyway: the ORIGINAL concern (a later
            # section commit reusing this stale received to satisfy its gate) is
            # already blocked by _review_evidence content-binding above -- a later
            # code commit whose staged blobs differ from the review's trigger_blobs
            # fails the binding regardless of the `received` flag. Resetting only
            # broke the legitimate stamp-only workflow (Canary #2 B3:
            # reset -> rollover refused -> receiving-review re-block -> repair loop).
            # The SKIP is still recorded in skip-log.jsonl above (the paper trail).
            sys.stderr.write(
                f"[section-commit-gate] SKIP allowed (stamp_only) -- "
                f"reason: {skip_reason}\n"
                f"[section-commit-gate]   logged to .claude/state/skip-log.jsonl; "
                f"last-codex-review.json received flag PRESERVED (stamp-only does "
                f"not invalidate the passed code review -- B3).\n"
            )
            return 0
        fourd_ok, fourd_err = _four_dispatch_evidence(root, flipped)
        if fourd_ok:
            return 0
        head = ""
        try:
            head = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=str(root),
                text=True, timeout=2, stderr=subprocess.DEVNULL,
            ).strip()
        except Exception:
            pass
        _emit_block(
            f"stamp-only commit detected on {flipped[0] if flipped else '(?)'} "
            f"but four-dispatch evidence is incomplete",
            [f"four-dispatch evidence: {fourd_err}"],
            head=head,
        )
        return 2

    # Real section commit. From here on, missing/malformed evidence
    # fails CLOSED.
    if skip_req and skip_reason:
        # Opt-out path. Audit + reset state, then allow.
        head = ""
        try:
            head = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=str(root),
                text=True, timeout=2, stderr=subprocess.DEVNULL,
            ).strip()
        except Exception:
            pass
        record = {
            "timestamp_ns": time.time_ns(),
            "mode": mode,
            "head_sha": head,
            "staged_sources": staged_src,
            "flipped_todos": flipped,
            "small_section_warn": _is_small_section(root, staged_src),
            "reason": skip_reason,
        }
        if not _skip_record_append(record, _skip_log_path(root)):
            sys.stderr.write(
                "[section-commit-gate] BLOCK -- SKIP audit write to "
                ".claude/state/skip-log.jsonl FAILED. The opt-out path "
                "requires a durable paper trail; refusing the commit.\n"
            )
            return 2
        _reset_review_state(root)
        sys.stderr.write(
            f"[section-commit-gate] SKIP allowed -- reason: {skip_reason}\n"
            f"[section-commit-gate]   logged to .claude/state/skip-log.jsonl; "
            f"last-codex-review.json reset (received: false).\n"
        )
        return 0

    # No-small-section-exemption: count source LOC delta and emit
    # a small-section WARN regardless of pass/fail outcome.
    small = _is_small_section(root, staged_src)
    if small:
        # WARN, not BLOCK. The user wants a sweep-able log line for
        # "small-section-skip-risk" patterns over time.
        warn_record = {
            "timestamp_ns": time.time_ns(),
            "mode": mode,
            "kind": "WARN",
            "tag": "small-section-skip-risk",
            "staged_sources": staged_src,
            "flipped_todos": flipped,
        }
        _skip_record_append(warn_record, _skip_log_path(root))

    # TODO-08 §21: WARN-first heuristic gates fire on every section
    # commit attempt that has an active implement-todo-section skill
    # window. They never block; they only emit stderr WARN + miss-log
    # entries to feed the WARN -> ERROR promotion pipeline.
    _emit_section21_heuristics(root, staged_src)

    # Evidence checks (Codex H2 fail-closed scope).
    build_ok, build_err = _build_evidence(root, staged_src)
    review_ok, review_err = _review_evidence(root, staged_src)
    # §5 four-dispatch check fires for any TODO file in the staged
    # diff that ADDS a stamp line, NOT just the section-flipping ones.
    # Codex M5 (post-impl): scanning only `flipped` missed cross-TODO
    # piggyback stamps (commit flips TODO-A while adding a stamp to
    # TODO-B; B's dispatches were never verified).
    all_staged_todos = _staged_todo_files(root)
    stamped = _stamped_todos(root, all_staged_todos)
    fourd_ok, fourd_err = _four_dispatch_evidence(root, stamped)
    # TODO-08 §17: re-adversarial trigger gate.
    re_adv_ok, re_adv_err = _re_adversarial_trigger_check(
        root, staged_src, all_staged_todos
    )
    # TODO-08 §20: impl-pipeline section-commit gates (steps 8/13/16).
    test_wiring_ok, test_wiring_err = _step8_test_wiring_check(
        root, staged_src, all_staged_todos
    )
    impl_adv_ok, impl_adv_err = _step13_impl_adversarial_check(
        root, staged_src, flipped, flipped_sections
    )
    smoke_ok, smoke_err = _step16_smoke_check(root, staged_src, cmd)
    if (build_ok and review_ok and fourd_ok and re_adv_ok
            and test_wiring_ok and impl_adv_ok and smoke_ok):
        return 0  # full evidence; commit allowed
    missing = []
    if not build_ok:
        missing.append(f"build evidence: {build_err}")
    if not review_ok:
        missing.append(f"Codex/receiving evidence: {review_err}")
    if not fourd_ok:
        missing.append(f"four-dispatch evidence: {fourd_err}")
    if not re_adv_ok:
        missing.append(f"re-adversarial evidence: {re_adv_err}")
    if not test_wiring_ok:
        missing.append(f"step-8 test-wiring evidence: {test_wiring_err}")
    if not impl_adv_ok:
        missing.append(f"step-13 impl-adversarial evidence: {impl_adv_err}")
    if not smoke_ok:
        missing.append(f"step-16 smoke-test evidence: {smoke_err}")
    head = ""
    try:
        head = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=str(root),
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        pass
    _emit_block(
        f"section-commit signature detected on {flipped[0] if flipped else '(?)'} "
        f"({len(staged_src)} source file(s) staged) but evidence is incomplete",
        missing,
        head=head,
    )
    return 2


# ----------------------------------------------------------------------
# TODO-08 §20: implement-pipeline section-commit gates (steps 8/13/16)
# ----------------------------------------------------------------------
# Each gate honors:
#   - SKIP_REVIEW_HOOK=1 (step 8 + step 13; reuses existing skip envelope)
#   - SKIP_SMOKE_GATE=1 + SKIP_SMOKE_GATE_REASON >=12 chars (step 16)
#   - _bootstrap_mode bypass when this hook's own file is in the staged
#     diff (already applied at _re_adversarial_trigger_check entry; the
#     §20 gates run AFTER that bypass so they inherit it via the same
#     return path -- if bootstrap-mode fires, _evaluate's full-evidence
#     check still passes because the new gates also return (True, "")
#     when their staged-source filter yields nothing).

_TEST_WIRING_SRC_PREFIXES = (
    "src/kernel/", "src/desktop/", "src/shell/", "src/apps/", "user/",
)
_TEST_WIRING_TEST_DIRS = {
    "src/kernel/": "src/kernel/test/",
    "src/desktop/": "src/desktop/test/",
    "src/shell/": "src/shell/test/",
    "src/apps/": "src/apps/test/",
    "user/": "user/test/",
}
_NO_TEST_SURFACE_RE = re.compile(
    r"\*\*Note:\*\*\s*No\s+\w+\s+test\s+surface", re.IGNORECASE
)


def _step8_test_wiring_check(
    root: Path, staged_src: list[str], todo_files: list[str]
) -> tuple[bool, str]:
    """TODO-08 §20 step-8: when staged diff touches src/{kernel,desktop,
    shell,apps}/ or user/, require ONE of:
      (a) staged diff also touches a `test_*.c` file in the matching
          test directory, OR
      (b) a `test_*.c` covering the same surface already exists at HEAD
          (Codex design M1: prior-commit coverage counts; same-commit
          test diff is too strict), OR
      (c) the section body contains `**Note:** No <surface> test surface`
          exemption phrase.
    SKIP_REVIEW_HOOK=1 honored (caller; same-skip path as build/review/
    four-dispatch). _bootstrap_mode honored when this hook's own file
    is in the staged diff (caller).
    """
    triggering_paths = [
        p for p in staged_src
        if any(p.startswith(pre) for pre in _TEST_WIRING_SRC_PREFIXES)
        and not any(td in p for td in _TEST_WIRING_TEST_DIRS.values())
    ]
    if not triggering_paths:
        return (True, "")
    # Skill-aware gate: only fire when implement-todo-section is active.
    skill_state_path = root / ".claude" / "state" / "skill-progress.json"
    if not skill_state_path.exists():
        return (True, "")
    try:
        skill_state = json.loads(skill_state_path.read_text(encoding="utf-8"))
    except Exception:
        return (True, "")
    if not isinstance(skill_state, dict):
        return (True, "")
    impl_entry = skill_state.get("implement-todo-section")
    if not isinstance(impl_entry, dict) or impl_entry.get("compaction_orphaned") is True:
        return (True, "")
    # Bootstrap-mode bypass for this gate (covers shipping the gate itself).
    try:
        from _bootstrap_mode import is_bootstrap_commit
        if is_bootstrap_commit(__file__, root):
            return (True, "")
    except Exception:
        pass

    # (a) Same-commit test diff matching the staged source basename.
    # Same H1 fix as (b): test_heap.c must not satisfy drivers/acpi.c.
    staged_tests = [p for p in staged_src
                    if "/test/" in p and re.search(r"test_[\w-]+\.c$", p)]
    for src_path in triggering_paths:
        src_base = os.path.basename(src_path)
        if "." in src_base:
            src_base = src_base.rsplit(".", 1)[0]
        base_pattern = re.compile(
            r"(^|/)test_" + re.escape(src_base) + r"(_[\w-]+)?\.c$"
        )
        if any(base_pattern.search(t) for t in staged_tests):
            return (True, "")

    # (b) Existing test_*.c at HEAD covering BASENAME of staged source.
    # Codex adversarial H1 fix: bind to source basename (e.g.
    # src/kernel/drivers/acpi.c -> test_acpi*.c) instead of accepting
    # any test_*.c in the broad surface directory. test_heap.c must
    # NOT satisfy a section that touches drivers/acpi.c.
    matched_existing = False
    for src_path in triggering_paths:
        # Extract basename without extension.
        src_base = os.path.basename(src_path)
        if "." in src_base:
            src_base = src_base.rsplit(".", 1)[0]
        # Find the matching test directory.
        test_dir = ""
        for src_pre, td in _TEST_WIRING_TEST_DIRS.items():
            if src_path.startswith(src_pre):
                test_dir = td
                break
        if not test_dir:
            continue
        try:
            out = subprocess.check_output(
                ["git", "ls-tree", "-r", "--name-only", "HEAD", test_dir],
                cwd=str(root), text=True, timeout=3,
                stderr=subprocess.DEVNULL,
            )
            # Match `test_<basename>` exactly OR `test_<basename>_*`
            # (e.g. test_acpi.c, test_acpi_smp.c -- both bind to
            # acpi.c). The .c suffix is required.
            base_pattern = re.compile(
                r"(^|/)test_" + re.escape(src_base) + r"(_[\w-]+)?\.c$"
            )
            if any(base_pattern.search(line) for line in out.splitlines()):
                matched_existing = True
                break
        except Exception:
            pass
    if matched_existing:
        return (True, "")

    # (c) Section-body exemption phrase in any staged TODO?
    for tf in todo_files:
        try:
            content = (root / tf).read_text(encoding="utf-8")
            if _NO_TEST_SURFACE_RE.search(content):
                return (True, "")
        except Exception:
            pass

    return (False, (
        f"staged diff touches {triggering_paths[0]} (and {len(triggering_paths)-1} "
        f"more) but neither a same-commit test_*.c diff, an existing "
        f"test_*.c at HEAD in the matching test directory, nor a "
        f"`**Note:** No <surface> test surface` exemption is present. "
        f"Wire a unit test or add the exemption phrase to the TODO section."
    ))


def _step13_impl_adversarial_check(
    root: Path, staged_src: list[str], flipped_todos: list[str],
    flipped_sections: dict[str, list[str]] | list[str] | None = None,
) -> tuple[bool, str]:
    """TODO-08 §20 step-13: when staged diff touches src/ AND IO row
    flips to [x] (i.e. flipped_todos non-empty), require an
    `adversarial` entry in last-review-stamps.json[<todo>] within
    30 minutes for at least one of the flipped TODOs. The legacy
    `adversarial-impl` alias/variant is accepted for older prompts.

    SKIP_REVIEW_HOOK=1 honored (caller). Bootstrap-mode honored.
    """
    if not staged_src or not flipped_todos:
        return (True, "")
    # Source-only filter: only fire when actual source code lands.
    has_src = any(
        p.endswith((".c", ".h", ".asm", ".S", ".py", ".sh", ".mjs"))
        for p in staged_src
    )
    if not has_src:
        return (True, "")
    # Skill-aware gate: only fire when implement-todo-section is the
    # active skill in skill-progress.json. Closes the test-fixture
    # regression where pre-§20 commit tests don't exercise the impl
    # flow but DO exercise the section-commit signature.
    skill_state_path = root / ".claude" / "state" / "skill-progress.json"
    if not skill_state_path.exists():
        return (True, "")
    try:
        skill_state = json.loads(skill_state_path.read_text(encoding="utf-8"))
    except Exception:
        return (True, "")
    if not isinstance(skill_state, dict):
        return (True, "")
    impl_entry = skill_state.get("implement-todo-section")
    if not isinstance(impl_entry, dict):
        return (True, "")
    if impl_entry.get("compaction_orphaned") is True:
        return (True, "")
    try:
        from _bootstrap_mode import is_bootstrap_commit
        if is_bootstrap_commit(__file__, root):
            return (True, "")
    except Exception:
        pass

    stamps_path = root / ".claude" / "state" / "last-review-stamps.json"
    if not stamps_path.exists():
        return (False, (
            f"IO row [x] flip on {flipped_todos[0]} requires step-13 "
            f"`[review-kind: adversarial]` Codex dispatch within the "
            f"last 30 minutes (`adversarial-impl` is accepted as a "
            f"legacy alias/variant); .claude/state/last-review-stamps.json "
            f"does not exist."
        ))
    try:
        state = json.loads(stamps_path.read_text(encoding="utf-8"))
    except Exception as exc:
        return (False, f"last-review-stamps.json malformed ({exc})")
    if not isinstance(state, dict):
        return (False, "last-review-stamps.json is not a JSON object")

    now_ns = time.time_ns()
    cutoff_ns = now_ns - 30 * 60 * 1_000_000_000

    # Codex adversarial H2 fix: bind evidence to the flipped section
    # AND the dispatch HEAD. Previously a fresh `adversarial-impl` for
    # a different section in the same TODO (or from another branch
    # within TTL) could satisfy the gate.
    current_head = ""
    try:
        current_head = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=str(root),
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        pass

    required_sections: list[tuple[str, str]] = []
    for todo in flipped_todos:
        if isinstance(flipped_sections, dict):
            sections_for_todo = [
                _normalize_section_id(sec)
                for sec in flipped_sections.get(todo, [])
                if _normalize_section_id(sec)
            ]
        else:
            sections_for_todo = [
                _normalize_section_id(sec)
                for sec in (flipped_sections or [])
                if _normalize_section_id(sec)
            ]
        sections_for_todo = sorted(set(sections_for_todo))
        if len(sections_for_todo) > 1:
            return (False, (
                f"IO row [x] flip on {todo} promotes multiple sections "
                f"({', '.join(sections_for_todo)}). Split the change into "
                f"one section commit per Codex review."
            ))
        if sections_for_todo:
            required_sections.append((todo, sections_for_todo[0]))

    def _entry_has_matching_impl_adv(todo: str, required_section: str) -> bool:
        entry = state.get(todo)
        if not isinstance(entry, dict):
            return False
        # Tag interchangeability (TODO-08 follow-up): `adversarial` is the
        # canonical implementation-time marker; `adversarial-impl` remains an
        # accepted alias/variant for older prompts. The head/section bindings
        # still apply per-tag.
        for tag in ("adversarial-impl", "adversarial"):
            ts = entry.get(tag)
            if not (isinstance(ts, int) and ts >= cutoff_ns):
                continue
            # Section binding: the recorded per-kind section must match
            # the section being flipped. Legacy entries may only have a
            # shared `section`; when the live path knows the flipped
            # section, missing or mismatched section evidence fails closed.
            recorded_section = _normalize_section_id(
                entry.get(f"{tag}_section") or entry.get("section", "")
            )
            if not recorded_section or recorded_section != required_section:
                continue
            # HEAD binding: the dispatch's `<tag>_head` must be an
            # ancestor of the current HEAD.
            rh = entry.get(f"{tag}_head", "")
            if isinstance(rh, str) and rh and current_head:
                if not _git_is_ancestor(root, rh, current_head):
                    continue
            return True
        return False

    if required_sections:
        missing = [
            f"{todo} §{section}"
            for todo, section in required_sections
            if not _entry_has_matching_impl_adv(todo, section)
        ]
        if not missing:
            return (True, "")
        return (False, (
            f"IO row [x] flip requires section-bound step-13 "
            f"`[review-kind: adversarial]` Codex evidence for each promoted "
            f"section; missing {', '.join(missing)}. Run: "
            f"`bash scripts/codex-dispatch.sh '[review-kind: adversarial] "
            f"<todo-path> <section-N> <prompt>'`."
        ))

    for todo in flipped_todos:
        entry = state.get(todo)
        if not isinstance(entry, dict):
            continue
        for tag in ("adversarial-impl", "adversarial"):
            ts = entry.get(tag)
            if not (isinstance(ts, int) and ts >= cutoff_ns):
                continue
            rh = entry.get(f"{tag}_head", "")
            if isinstance(rh, str) and rh and current_head:
                if not _git_is_ancestor(root, rh, current_head):
                    continue
            return (True, "")
    return (False, (
        f"IO row [x] flip on {flipped_todos[0]} requires step-13 "
        f"`[review-kind: adversarial]` Codex dispatch "
        f"(`adversarial-impl` is accepted as a legacy alias/variant). "
        f"Evidence is bound to the section number and dispatch HEAD "
        f"ancestry. Run: "
        f"`bash scripts/codex-dispatch.sh '[review-kind: adversarial] "
        f"<todo-path> <section-N> <prompt>'`."
    ))


_SMOKE_BOOT_PATH_GLOBS = (
    "src/boot/", "src/kernel/main/boot_",
    "src/kernel/idt.c", "src/kernel/gdt.c", "src/kernel/msr.c",
    "src/kernel/smp/",
    "src/kernel/mm/pmm.c", "src/kernel/mm/vmm.c", "src/kernel/mm/heap.c",
    "src/kernel/drivers/lapic.c", "src/kernel/drivers/ioapic.c",
    "src/kernel/drivers/acpi.c", "src/kernel/drivers/timer.c",
)


def _step16_smoke_check(
    root: Path, staged_src: list[str], cmd: str = ""
) -> tuple[bool, str]:
    """TODO-08 §20 step-16: when staged diff touches a boot-path glob,
    require build/smoke-test.stripped.log to exist, be more recent than
    the most-recent staged file mtime, AND contain BOTH `Boot complete`
    AND `C:\\>` markers in the body (Codex design H2: stdout PASS banner
    is NOT in the stripped log; the script validates serial markers).

    Opt-out:
        SKIP_SMOKE_GATE=1 SKIP_SMOKE_GATE_REASON="<text >= 12 chars>"
    Bootstrap-mode honored.
    """
    boot_paths = [
        p for p in staged_src
        if any(p.startswith(g) for g in _SMOKE_BOOT_PATH_GLOBS)
    ]
    if not boot_paths:
        return (True, "")
    # Skill-aware gate: only fire when implement-todo-section is active.
    skill_state_path = root / ".claude" / "state" / "skill-progress.json"
    if not skill_state_path.exists():
        return (True, "")
    try:
        skill_state = json.loads(skill_state_path.read_text(encoding="utf-8"))
    except Exception:
        return (True, "")
    if not isinstance(skill_state, dict):
        return (True, "")
    impl_entry = skill_state.get("implement-todo-section")
    if not isinstance(impl_entry, dict) or impl_entry.get("compaction_orphaned") is True:
        return (True, "")
    try:
        from _bootstrap_mode import is_bootstrap_commit
        if is_bootstrap_commit(__file__, root):
            return (True, "")
    except Exception:
        pass
    # SKIP_SMOKE_GATE opt-out (separate from SKIP_REVIEW_HOOK).
    # TODO-08 §23 unification (Codex consistency M1 fix 2026-04-29):
    # read via shared helper so inline `SKIP_SMOKE_GATE=1 git commit ...`
    # form works the same as harness env. Inline wins on collision.
    if str(_HOOK_DIR) not in sys.path:
        sys.path.insert(0, str(_HOOK_DIR))
    import _skip_env as _se  # noqa: E402
    _smoke_envs = _se.read_skip_envs(
        cmd,
        keys=("SKIP_SMOKE_GATE", "SKIP_SMOKE_GATE_REASON"),
        fallback_to_environ=True,
    )
    if _smoke_envs.get("SKIP_SMOKE_GATE", "") == "1":
        reason = _smoke_envs.get("SKIP_SMOKE_GATE_REASON", "")
        if len(reason) < SKIP_REASON_MIN_LEN:
            return (False, (
                f"SKIP_SMOKE_GATE=1 set but SKIP_SMOKE_GATE_REASON "
                f"missing or under {SKIP_REASON_MIN_LEN} chars."
            ))
        return (True, "")

    smoke_log = root / "build" / "smoke-test.stripped.log"
    if not smoke_log.exists():
        return (False, (
            f"staged boot-path edit ({boot_paths[0]}) requires "
            f"`bash scripts/test-smoke.sh` PASS first; "
            f"build/smoke-test.stripped.log does not exist."
        ))
    try:
        log_mtime = smoke_log.stat().st_mtime
    except Exception:
        return (False, "smoke-test.stripped.log mtime unreadable")
    # Compare against most-recent staged file mtime.
    # Codex adversarial M1 fix: fail closed when stat() fails -- a
    # boot-path file staged as deleted/renamed-out would silently leave
    # most_recent=0, letting any old smoke log pass freshness. Now: any
    # stat failure means we cannot prove freshness and BLOCK.
    most_recent = 0.0
    for p in boot_paths:
        full = root / p
        try:
            mt = full.stat().st_mtime
            if mt > most_recent:
                most_recent = mt
        except FileNotFoundError:
            return (False, (
                f"staged boot-path {p} is not present in the worktree "
                f"(deleted/renamed staged change). Cannot verify smoke "
                f"freshness; re-run `bash scripts/test-smoke.sh` against "
                f"the post-rename layout, or use SKIP_SMOKE_GATE."
            ))
        except Exception as exc:
            return (False, (
                f"stat() failed on staged boot-path {p}: {exc}. Cannot "
                f"verify smoke freshness; re-run smoke or use SKIP."
            ))
    if log_mtime < most_recent:
        return (False, (
            f"build/smoke-test.stripped.log is older than the staged "
            f"boot-path file(s). Re-run `bash scripts/test-smoke.sh`."
        ))
    # Validate serial markers (Codex design H2 fix).
    try:
        body = smoke_log.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return (False, "smoke-test.stripped.log unreadable")
    if "Boot complete" not in body or "C:\\>" not in body:
        return (False, (
            f"build/smoke-test.stripped.log does not contain BOTH "
            f"`Boot complete` and `C:\\>` markers; smoke test did not "
            f"reach userspace shell. Re-run."
        ))
    return (True, "")


def _index_is_nonempty(root: Path) -> bool:
    """True if `git diff --cached` reports any staged change. Used
    by the H2 fail-closed path: an empty index means the commit
    will fail anyway (or is `--allow-empty`); a non-empty index
    means we must not fail open in the load-bearing git-hook layer.
    """
    try:
        subprocess.check_output(
            ["git", "diff", "--cached", "--quiet"],
            cwd=str(root), timeout=3, stderr=subprocess.DEVNULL,
        )
        return False  # exit 0 = no changes
    except subprocess.CalledProcessError as exc:
        # exit 1 = changes present; any other code = treat as nonempty.
        return exc.returncode != 0
    except Exception:
        # Cannot tell -- be conservative in the load-bearing layer.
        return True


def _is_small_section(root: Path, staged_src: list[str]) -> bool:
    """True if staged source delta is < 50 LOC (any change, +/-).
    Codex M1 + user feedback: small sections are the highest-risk
    skip pattern. The gate does NOT relax for small diffs; this just
    drops a skip-log WARN line.
    """
    try:
        out = subprocess.check_output(
            ["git", "diff", "--cached", "--numstat", "--", *staged_src],
            cwd=str(root), text=True, timeout=5, stderr=subprocess.DEVNULL,
        )
    except Exception:
        return False
    total = 0
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        added = parts[0]
        removed = parts[1]
        try:
            total += int(added) if added != "-" else 0
            total += int(removed) if removed != "-" else 0
        except ValueError:
            continue
    return total < 50


# ============================================================================
# TODO-08 §21: WARN-first heuristic gates (steps 9, 15, 17, 18).
# ============================================================================
# Each helper returns None and emits via _heuristic_misses.emit_warn when the
# heuristic fires. None of them BLOCK; they're advisory and feed the
# heuristic-misses.jsonl ratio dataset for the WARN -> ERROR promotion path
# documented in docs/infrastructure/ai-system.md "Hook Promotion Pipeline".

def _heuristic_load_helpers():
    """Lazy import of _heuristic_misses + tool-history reader. Returns
    (hm_module, history_list_for_window). Returns (None, []) on any
    error so heuristic emission is best-effort and never blocks the
    commit-gate path."""
    try:
        if str(_HOOK_DIR) not in sys.path:
            sys.path.insert(0, str(_HOOK_DIR))
        import _heuristic_misses as _hm  # noqa: E402
        return _hm
    except Exception:
        return None


def _heuristic_read_tool_history(root: Path, since_ts_ns: int) -> list:
    """Codex re-adversarial #2 M1 fix 2026-04-29: read rotated .1
    BEFORE live so chronological order is preserved across the
    10 MiB rotation boundary. Otherwise long sessions misclassify
    pre-rotation Codex/edit evidence as missing."""
    p_live = root / ".claude" / "state" / "tool-history.jsonl"
    p_rot = root / ".claude" / "state" / "tool-history.jsonl.1"
    out = []
    try:
        for path in (p_rot, p_live):
            if not path.exists():
                continue
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                try:
                    rec = json.loads(line)
                except Exception:
                    continue
                if not isinstance(rec, dict):
                    continue
                ts = rec.get("ts_ns", 0)
                if isinstance(ts, int) and ts > since_ts_ns:
                    out.append(rec)
    except Exception:
        return []
    return out


def _heuristic_active_skill_window(root: Path) -> tuple:
    """Return (skill_name, started_ts_ns, todo_path, section_id) for
    the most-recent active multi-step skill, or ('', 0, '', None)."""
    try:
        p = root / ".claude" / "state" / "skill-progress.json"
        if not p.exists():
            return ("", 0, "", None)
        state = json.loads(p.read_text(encoding="utf-8"))
        if not isinstance(state, dict):
            return ("", 0, "", None)
        best = None
        best_ts = -1
        for name, entry in state.items():
            if not isinstance(entry, dict):
                continue
            if entry.get("compaction_orphaned") is True:
                continue
            if name not in (
                "implement-todo-section", "review-todo-section",
                "verify-todo-section",
            ) and not name.startswith("implement-todo-section"):
                continue
            ts = entry.get("started_ts", 0)
            if isinstance(ts, int) and ts > best_ts:
                best_ts = ts
                best = (name, entry)
        if not best:
            return ("", 0, "", None)
        name, entry = best
        todo_path = entry.get("todo_path", "") or ""
        section = entry.get("section")
        if not isinstance(section, int):
            section = None
        return (name, best_ts, todo_path, section)
    except Exception:
        return ("", 0, "", None)


def _heuristic_step9_test_coverage(root: Path, staged_src: list, todo_path: str,
                                    section) -> None:
    """Step 9: WARN if staged diff adds >=3 new TEST_ASSERT/TEST_PENDING/
    TEST_SKIP lines in test_*.c files but no `[review-kind: test-coverage]`
    Codex dispatch is recorded in last-review-stamps.json within 30 min."""
    hm = _heuristic_load_helpers()
    if hm is None:
        return
    try:
        # Count new test-assertion lines in staged test_*.c files.
        added = 0
        test_files = [s for s in staged_src
                      if s.startswith("src/kernel/test/test_") and s.endswith(".c")]
        if not test_files:
            return
        for f in test_files:
            try:
                diff = subprocess.check_output(
                    ["git", "diff", "--cached", "--unified=0", "--", f],
                    cwd=str(root), text=True, timeout=5,
                    stderr=subprocess.DEVNULL,
                )
            except Exception:
                continue
            for line in diff.splitlines():
                if not line.startswith("+") or line.startswith("+++"):
                    continue
                if "TEST_ASSERT" in line or "TEST_PENDING" in line or "TEST_SKIP" in line:
                    added += 1
        if added < 3:
            return
        # Look for a recent test-coverage dispatch in last-review-stamps.json.
        stamps_path = root / ".claude" / "state" / "last-review-stamps.json"
        if not stamps_path.exists():
            stamps = {}
        else:
            try:
                stamps = json.loads(stamps_path.read_text(encoding="utf-8"))
            except Exception:
                stamps = {}
        now_ns = time.time_ns()
        thirty_min_ns = 30 * 60 * 1_000_000_000
        found = False
        if isinstance(stamps, dict) and todo_path:
            entry = stamps.get(todo_path) or {}
            if isinstance(entry, dict):
                # Codex review M1 fix 2026-04-29: codex_review_completed
                # ._record_stamp writes every kind as a bare int timestamp
                # (entry[kind] = now_ns). Original code treated this slot
                # as a {ts_ns: int} dict and never matched, causing every
                # test-touching commit to false-positive. Accept both shapes.
                tc = entry.get("test-coverage")
                ts = 0
                if isinstance(tc, int):
                    ts = tc
                elif isinstance(tc, dict):
                    inner = tc.get("ts_ns", 0)
                    if isinstance(inner, int):
                        ts = inner
                if ts > 0 and (now_ns - ts) <= thirty_min_ns:
                    found = True
        if not found:
            hm.emit_warn(
                str(root), 9,
                "test-coverage-missing",
                "staged diff adds " + str(added) + " new test "
                "assertions in " + str(len(test_files)) + " file(s) but "
                "no [review-kind: test-coverage] Codex dispatch recorded "
                "within the last 30 minutes for " + (todo_path or "<no active todo>"),
                todo_path=todo_path, section=section,
            )
    except Exception:
        pass


def _heuristic_step15_post_codex_edit(root: Path, history: list,
                                       todo_path: str,
                                       section) -> None:
    """Step 15: WARN if last Codex dispatch happened but no Edit/Write
    happened between that dispatch and now (suggests no fix loop ran).
    Codex perf M2 fix: history is pre-read by the driver."""
    hm = _heuristic_load_helpers()
    if hm is None:
        return
    try:
        latest_codex_ts = 0
        for r in history:
            if r.get("tool_name") != "Bash":
                continue
            cmd = r.get("target", "") or ""
            if "codex-companion.mjs" in cmd or "codex exec" in cmd or "codex review" in cmd:
                ts = r.get("ts_ns", 0)
                if isinstance(ts, int) and ts > latest_codex_ts:
                    latest_codex_ts = ts
        if latest_codex_ts == 0:
            return  # no Codex dispatch in this skill window; not in scope
        # Look for any Edit/Write/MultiEdit after latest_codex_ts.
        post_codex_edit = any(
            r.get("tool_name") in ("Edit", "Write", "MultiEdit")
            and isinstance(r.get("ts_ns", 0), int)
            and r.get("ts_ns", 0) > latest_codex_ts
            for r in history
        )
        if not post_codex_edit:
            hm.emit_warn(
                str(root), 15,
                "no-edit-after-codex",
                "section commit pending but no Edit/Write recorded since "
                "the latest Codex dispatch (suggests no fix loop ran). If "
                "Codex returned zero findings, that is the explicit stamp "
                "covering this case.",
                todo_path=todo_path, section=section,
            )
    except Exception:
        pass


def _heuristic_step17_validate_phase(root: Path, history: list,
                                      todo_path: str,
                                      section) -> None:
    """Step 17: WARN if fewer than 2 Read/Grep calls happened between
    the latest Codex dispatch and now (suggests no validate phase).
    Codex perf M2 fix: history is pre-read by the driver."""
    hm = _heuristic_load_helpers()
    if hm is None:
        return
    try:
        latest_codex_ts = 0
        for r in history:
            if r.get("tool_name") != "Bash":
                continue
            cmd = r.get("target", "") or ""
            if "codex-companion.mjs" in cmd or "codex exec" in cmd or "codex review" in cmd:
                ts = r.get("ts_ns", 0)
                if isinstance(ts, int) and ts > latest_codex_ts:
                    latest_codex_ts = ts
        if latest_codex_ts == 0:
            return
        validate_calls = sum(
            1 for r in history
            if r.get("tool_name") in ("Read", "Grep")
            and isinstance(r.get("ts_ns", 0), int)
            and r.get("ts_ns", 0) > latest_codex_ts
        )
        if validate_calls < 2:
            hm.emit_warn(
                str(root), 17,
                "validate-thin",
                "section commit pending after only " + str(validate_calls)
                + " Read/Grep call(s) since latest Codex dispatch; "
                "implement-todo-section step 17 expects a validate phase "
                "that re-checks the diff against code-truth.",
                todo_path=todo_path, section=section,
            )
    except Exception:
        pass


def _heuristic_step18_loose_ends_scan(root: Path, history: list,
                                        staged_src: list,
                                        todo_path: str,
                                        section) -> None:
    """Step 18: WARN if no Grep matching TODO/FIXME/HACK/STATUS_NOT_IMPLEMENTED
    is recorded between latest src/ edit and now. Suppress if staged diff
    is markdown-only (docs/stamp-only commits don't need this scan).
    Codex perf M2 fix: history is pre-read by the driver."""
    hm = _heuristic_load_helpers()
    if hm is None:
        return
    try:
        # Suppress on docs-only / stamp-only.
        non_md = [s for s in staged_src
                  if not (s.endswith(".md") or s.startswith("todo/"))]
        if not non_md:
            return
        latest_src_edit_ts = 0
        for r in history:
            if r.get("tool_name") not in ("Edit", "Write", "MultiEdit"):
                continue
            t = r.get("target", "") or ""
            if t.startswith("src/") or t.startswith("include/") \
               or "/src/" in t or "/include/" in t:
                ts = r.get("ts_ns", 0)
                if isinstance(ts, int) and ts > latest_src_edit_ts:
                    latest_src_edit_ts = ts
        if latest_src_edit_ts == 0:
            return
        scan_grep = any(
            r.get("tool_name") == "Grep"
            and isinstance(r.get("ts_ns", 0), int)
            and r.get("ts_ns", 0) > latest_src_edit_ts
            and isinstance(r.get("target", ""), str)
            and re.search(r"TODO|FIXME|HACK|STATUS_NOT_IMPLEMENTED",
                          r.get("target", ""))
            for r in history
        )
        if not scan_grep:
            hm.emit_warn(
                str(root), 18,
                "loose-ends-scan-missing",
                "section commit pending but no `Grep` for "
                "TODO|FIXME|HACK|STATUS_NOT_IMPLEMENTED recorded since "
                "the last src/ edit. implement-todo-section step 18: "
                "scan for stubs/markers before marking the section done.",
                todo_path=todo_path, section=section,
            )
    except Exception:
        pass


def _emit_section21_heuristics(root: Path, staged_src: list) -> None:
    """Driver: fire all 4 section-commit-gate heuristics (steps 9, 15, 17, 18)
    if there is an active implement-todo-section skill window.

    Codex perf M2 fix 2026-04-29: read tool-history.jsonl ONCE here
    and pass the filtered list into each heuristic. The previous shape
    re-parsed the same file 3x per commit; at the 10 MiB live cap that
    was ~30 MiB of synchronous parse on every section-commit attempt."""
    try:
        skill, started_ts, todo_path, section = _heuristic_active_skill_window(root)
        if skill != "implement-todo-section" or started_ts <= 0:
            return
        history = _heuristic_read_tool_history(root, started_ts)
        _heuristic_step9_test_coverage(root, staged_src, todo_path, section)
        _heuristic_step15_post_codex_edit(root, history, todo_path, section)
        _heuristic_step17_validate_phase(root, history, todo_path, section)
        _heuristic_step18_loose_ends_scan(root, history, staged_src,
                                            todo_path, section)
    except Exception:
        pass


def main(argv: list[str]) -> int:
    if "--git-hook-mode" in argv:
        root = _repo_root()
        if root is None:
            return 0  # not a git context; fail open
        return _git_hook_mode_main(root)
    return _harness_main()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
