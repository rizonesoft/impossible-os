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

import collections
import functools
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


def _walk_wrappers(words: list[str]) -> tuple[list[str], str | None]:
    """Strip command-runner wrappers (`sudo -u x`, `timeout 5`, `/usr/bin/env FOO=1`,
    `flock f`) from the front of a command. Returns the words of the command that runs,
    and the script of a `flock -c` (the words are then empty)."""
    out = collections.deque(words)  # popleft: pop(0) shifted every token (14.7 s at 320000)
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

    while out and out[0].rsplit("/", 1)[-1] in _WRAPPER_TOKENS:
        wrapper = out.popleft().rsplit("/", 1)[-1]  # /usr/bin/env is env
        # flock special-case (Codex H2 round-4): `flock -c "cmd" file`
        # OR `flock file -c "cmd"` runs cmd as a shell. `flock -n file
        # cmd args` runs cmd with args. Both forms have a lockfile/fd
        # positional that must NOT be treated as the command head.
        if wrapper == "flock":
            inner_cmd = None
            consumed_lockfile = False
            while out and (out[0].startswith("-") or not consumed_lockfile):
                if out[0].startswith("-"):
                    flag = out.popleft()
                    if "=" in flag:
                        head_eq, val_eq = flag.split("=", 1)
                        if head_eq in ("-c", "--command"):
                            inner_cmd = val_eq
                        continue
                    if flag in ("-c", "--command") and out:
                        inner_cmd = out.popleft()
                        continue
                    if flag in ("-w", "--timeout", "-E",
                                "--conflict-exit-code") and out:
                        out.popleft()
                        continue
                    # bare flock flags: -s/-x/-u/-n/-o/-F/-E/-h/-V (no value)
                    continue
                # Non-flag positional. First positional is the lockfile
                # or fd; consume it once, then exit so the real command
                # head can be processed by the outer logic.
                if not consumed_lockfile:
                    out.popleft()
                    consumed_lockfile = True
                    continue
                break
            if inner_cmd is not None:
                return [], inner_cmd  # -c: the command is that script, nothing follows
            continue  # flock with no -c -- proceed to next wrapper iter
        value_flags = _WRAPPER_VALUE_FLAGS.get(wrapper, frozenset())
        while out and out[0].startswith("-"):
            flag = out.popleft()
            if "=" in flag:
                continue
            if flag in value_flags and out:
                out.popleft()
        # Wrapper-specific positional args before the real command.
        if wrapper == "timeout" and out and _is_duration(out[0]):
            out.popleft()
        elif wrapper == "taskset" and out and _looks_like_cpu_mask(out[0]):
            # `taskset MASK cmd args` -- MASK is hex/decimal bitmask
            # (e.g. 03, 0x3, ff). With -p it's a PID after -p flag
            # (already consumed). With -c it's a list after -c (also
            # already consumed). Round-5 fix.
            out.popleft()
        elif wrapper == "env":
            # `env FOO=bar BAZ=qux git commit ...` -- consume KEY=VAL pairs.
            while out and "=" in out[0] and not out[0].startswith("="):
                head_a = out[0].split("=", 1)[0]
                if head_a and (head_a[0].isalpha() or head_a[0] == "_") and all(
                    c.isalnum() or c == "_" for c in head_a
                ):
                    out.popleft()
                else:
                    break
        elif wrapper == "nice" and out and out[0].lstrip("-").isdigit():
            out.popleft()
        elif wrapper == "chrt" and out and out[0].isdigit():
            out.popleft()
    return list(out), None


# Characters one top-level reading may spend re-reading the scripts its commands run.
# Each `eval` re-reads what follows it and the lexer's depth starts again inside, so
# `eval eval ... <200 KB>` cost payload x depth: 10 s at 160 levels.
_REREAD_BUDGET = 2_000_000
_REREAD_LEFT = _REREAD_BUDGET


def _inner_script_is_git_commit(script: str) -> tuple[bool, bool]:
    """A script a command runs again (`bash -c`, `eval`), read with the same reader as
    the outer command. Nesting too deep, or more re-reading than the budget allows, is
    judged unread: a possible commit is refused."""
    global _REREAD_LEFT
    if script not in (_COMMIT_MEMO or {}):
        _REREAD_LEFT -= len(script)
        if _REREAD_LEFT < 0:
            return _unread_commit(script)
    try:
        return _bash_is_git_commit(script)
    except RecursionError:
        return _unread_commit(script)


# git commit's options as its parse-options reads them (`git commit
# --git-completion-helper-all`, git 2.53): "b" takes no value, "r" a required one (the
# next argument, or after `=`), "o" an optional one (after `=` only).
_COMMIT_OPTIONS: dict[str, str] = {}
for _names, _kind in (
        ("quiet verbose reset-author signoff edit status all include interactive patch only "
         "dry-run short branch ahead-behind porcelain long null amend pathspec-file-nul "
         "allow-empty allow-empty-message", "b"),
        ("file author date message reedit-message reuse-message fixup squash template "
         "cleanup pathspec-from-file", "r"),
        ("gpg-sign untracked-files", "o")):
    for _name in _names.split():
        _COMMIT_OPTIONS[_name] = _kind
        _COMMIT_OPTIONS["no-" + _name] = "b"
_COMMIT_OPTIONS.update({"trailer": "r", "unified": "r", "inter-hunk-context": "r",
                        "no-verify": "b", "verify": "b", "no-post-rewrite": "b", "post-rewrite": "b"})
_COMMIT_SHORT = {**dict.fromkeys("qvseaipozn", "b"), **dict.fromkeys("FmcCtU", "r"), "S": "o", "u": "o"}
_COMMIT_SHORT_LONG = {"n": "no-verify"}


def _commit_skips_verify(args: list[str]) -> bool:
    """Whether `git commit <args>` skips pre-commit, read as git's parse-options reads
    them: a value option takes its value (`-m -n` is a message, `-mn` too), `--` and
    `--end-of-options` end the options, a long option may be any unique prefix
    (`--no-veri`), and the last of `--no-verify` and `--verify` wins. Text git itself
    would refuse (an unknown or ambiguous option, a value on a flag) is read as skipping:
    a false refusal costs one retry, a missed one disables the gate."""
    skip = False
    i = 0
    while i < len(args):
        arg = args[i]
        i += 1
        if arg in ("--", "--end-of-options"):
            break
        if arg.startswith("--"):
            name, eq, _value = arg[2:].partition("=")
            if name in _COMMIT_OPTIONS:
                hits = [name]
            else:
                hits = [o for o in _COMMIT_OPTIONS if o.startswith(name)] if name else []
            if len(hits) != 1:
                return True
            kind = _COMMIT_OPTIONS[hits[0]]
            if kind == "b" and eq:
                return True
            if kind == "r" and not eq:
                i += 1  # the next argument is the value, whatever it reads
            if hits[0] in ("no-verify", "verify"):
                skip = hits[0] == "no-verify"
            continue
        if arg.startswith("-") and len(arg) > 1:
            for k, ch in enumerate(arg[1:], 1):
                kind = _COMMIT_SHORT.get(ch)
                if kind is None:
                    return True
                if _COMMIT_SHORT_LONG.get(ch) == "no-verify":
                    skip = True
                if kind == "r":
                    if k == len(arg) - 1:
                        i += 1  # `-m`: the next argument is the message
                    break  # `-mfoo`: the rest of the word is the message
                if kind == "o":
                    break  # `-Skey`: the rest of the word is the key
        # anything else is a pathspec; git reads options after it too
    return skip


def _segment_is_git_commit(seg_tokens: list[str]) -> tuple[bool, bool]:
    """Returns (is_git_commit, has_no_verify). Walks env-prefix +
    wrapper tokens, then handles git's own pre-subcommand options
    (`-c key=value`, `-C path`, `--git-dir=...`, etc.) before
    matching the subcommand. Also flags `--no-verify`/`-n` (Codex
    H1: --no-verify skips .githooks/pre-commit entirely, so the
    harness layer must catch it).
    """
    # Strip env-prefix assignments: counted, then sliced once (pop(0) per assignment
    # shifted the rest every time).
    k = 0
    while k < len(seg_tokens) and "=" in seg_tokens[k] and not seg_tokens[k].startswith("="):
        head = seg_tokens[k].split("=", 1)[0]
        if head and (head[0].isalpha() or head[0] == "_") and all(
            c.isalnum() or c == "_" for c in head
        ):
            k += 1
        else:
            break
    out = list(seg_tokens[k:])
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
            return _inner_script_is_git_commit(out[idx])  # read as bash reads it
    # `eval "git commit --no-verify -m foo"` -- eval re-shells its arg.
    # Concatenate all positional args (eval joins with spaces in POSIX
    # shells) and re-tokenize. Codex H2 round-4 fix.
    if out and out[0].rsplit("/", 1)[-1] == "eval":
        return _inner_script_is_git_commit(" ".join(out[1:]))
    walked, flock_script = _walk_wrappers(out)
    if flock_script is not None:
        return _inner_script_is_git_commit(flock_script)
    if walked != out and walked and walked[0].rsplit("/", 1)[-1] in _SHELLS | {"eval"}:
        return _segment_is_git_commit(walked)  # `timeout 5 bash -c "..."`
    out = walked
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
    commit_args = list(out[idx + 1:])  # an alias may put its own arguments first
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
                # The alias's own arguments come first and the command's follow them.
                commit_args = alias_toks[a_idx + 1:] + commit_args
            if not extra_no_verify:
                for tok in alias_toks[:a_idx]:
                    if tok in ("--no-verify", "-n") or tok.startswith("--no-verify="):
                        extra_no_verify = True
                        break
    if subcmd not in ("commit", "ci", "cm"):
        return (False, False)
    return (True, extra_no_verify or _commit_skips_verify(commit_args))


_ANSI_C = {"n": "\n", "t": "\t", "r": "\r", "e": "\x1b", "E": "\x1b", "a": "\a", "b": "\b",
           "f": "\f", "v": "\v", "\\": "\\", "'": "'", '"': '"', "?": "?"}


def _ansi_c_escape(s: str, k: int) -> tuple[str, int]:
    """Decode the escape at s[k] (a backslash) inside `$'...'` as bash does: \\xHH,
    \\NNN octal, \\uHHHH, \\UHHHHHHHH, \\cX and the single letters. Returns (text, next k)."""
    nx = s[k + 1]
    hexd = "0123456789abcdefABCDEF"
    for lead, width in (("x", 2), ("u", 4), ("U", 8)):
        if nx == lead:
            j = k + 2
            while j < len(s) and j < k + 2 + width and s[j] in hexd:
                j += 1
            if j == k + 2:
                return "\\" + nx, k + 2
            try:
                return chr(int(s[k + 2:j], 16)), j
            except (ValueError, OverflowError):
                return "", j
    if nx in "01234567":
        j = k + 1
        while j < len(s) and j < k + 4 and s[j] in "01234567":
            j += 1
        return chr(int(s[k + 1:j], 8) & 0xFF), j
    if nx == "c" and k + 2 < len(s):
        if s.startswith("\\\\", k + 2):  # \c\\ is control-backslash: both backslashes go
            return chr(0x1C), k + 4
        return chr(ord(s[k + 2]) & 0x1F), k + 3
    return _ANSI_C.get(nx, "\\" + nx), k + 2


# How deep substitutions and expansions may nest before the reading gives up. Past it
# the whole command is read conservatively (_unread_commit), never crashes the hook.
_MAX_NESTING = 24


class _TooDeep(Exception):
    """Nesting past _MAX_NESTING."""


class _Op(str):
    """A shell control operator. Only these split commands: a word that merely reads `;`
    (`$'\\x3b'`, `';'`) is an argument and stays one."""


_SEP = _Op(";")
_SHELLS = frozenset(("bash", "sh", "zsh", "dash", "ksh"))
_SUBSCRIPT_OPEN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\[")
_COMPOUND_ASSIGN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\+?=")


def _strip_wrappers(words: list[str]) -> list[str]:
    """`if FOO=1 timeout 5 /usr/bin/env -u X bash`: the words from the command that
    runs, through the same wrapper walk the commit reader uses."""
    k = 0
    while k < len(words) and (words[k] in _RESERVED_LEADERS
                              or ("=" in words[k] and not words[k].startswith("-"))):
        k += 1
    out, flock_script = _walk_wrappers(words[k:])
    return [] if flock_script is not None else out


def _commands(toks: list[str]) -> list[list[str]]:
    """The simple commands of a token list, split only at real operators."""
    out: list[list[str]] = [[]]
    for tok in toks:
        if isinstance(tok, _Op):
            if out[-1]:
                out.append([])
        else:
            out[-1].append(tok)
    return [c for c in out if c]


class _BashLexer:
    """Words and control operators of a bash script, read as bash reads them.

    shlex knows neither `$'...'`, a quote that spans lines, a heredoc, a comment nor a
    command substitution, and every reading built on it was wrong somewhere: a line it
    could not tokenize hid its commit (`git commit -m $'a\\';b' -n`), and a word
    starting with `$(` cut the rest of a commit off (`git commit -m "$(cat f)" -n`).
    This reads the script once, in bash's own quoting. A heredoc body is dropped, since
    bash runs none of it; the text of a substitution stays in its word, and its contents
    are read as commands too, since bash runs them. `subs` collects the text of those
    substitutions; the caller reads each as a script after the script's own.
    """

    def __init__(self, src: str, depth: int = 0):
        if depth > _MAX_NESTING:
            raise _TooDeep
        self.depth = depth
        self.s, self.n, self.i = src, len(src), 0
        self.toks: list[str] = []
        self.subs: list[str] = []
        # [delim, strip, quoted, is_script]; is_script is settled when its command ends.
        self.heredocs: list[list] = []
        self.cmd_heredocs: list[int] = []  # heredocs of the command being read
        self.cmd_herestrings: list[str] = []  # and its `<<<` words
        self.herestring_next = False
        self.skip_operand = False
        self.stop_paren = False  # lex only up to the `)` that closes a substitution
        # False in a lexer that only finds where a substitution closes: what it would
        # collect is read again from the body, and collecting it here too made the
        # work grow exponentially with nesting (`"a[$("a[$(...)]")]"`).
        self.collect = True
        # The `)` matching each `(` a `_close` scan has passed. A later scan from inside
        # one is that scan's continuation, so it is answered from here: `(( (( ... ) ) ) )`
        # rescanned the nest once per opener (8.5 s at 5500).
        self.paren_match: dict[int, int] = {}
        self.stop_at = -1
        self.parens = 0
        self.cases = 0
        # Within a case: 1 its subject is next, 2 `in` is next, 3 a pattern list starts
        # (where only `esac` is a keyword), 4 inside one (after `(`, `|` or a word, where
        # `esac` is a pattern too); `)` ends it. 0 elsewhere.
        self.case_state = 0
        self.word_quoted = False
        self.pattern = False  # an extglob interior: `#` is text, not a comment
        self.compound = False  # inside `a=( ... )`, whose `[key]=v` elements are subscripts

    def _expand(self, *args, **kw) -> None:
        """Collect the commands inside `_expansions(...)` unless this lexer only finds a close."""
        if self.collect:
            self.subs.extend(_expansions(*args, **kw))

    def _subscript(self, text: str) -> None:
        """`a['$(cmd)']=x`, `unset 'a[$(cmd)]'`, `read a'[$''(cmd)]'`: an array subscript
        runs its substitutions through quotes. Which words name an array is not in the
        text, and where a subscript ends is decided by quotes already removed here, so a
        quoted word that opens with `name[` is read whole as running them (a false
        refusal at worst). A prompt '[review-kind: x] ...' opens with no name."""
        m = _SUBSCRIPT_OPEN_RE.match(text)
        if m:
            self._expand(text[m.end():], self.depth + 1)
        elif self.compound and text.startswith("["):  # `a=( ['$(cmd)']=x )`
            self._expand(text[1:], self.depth + 1)

    def _case_word(self, text: str) -> None:
        """Follow `case WORD in PATTERN) ... ;; esac` so that a pattern's `)` is not
        read as the end of the substitution. A quoted word is never a keyword."""
        keyword = not self.word_quoted
        command = not self.toks or isinstance(self.toks[-1], _Op) or self.toks[-1] in _RESERVED_LEADERS
        if self.case_state == 1:
            self.case_state = 2
        elif self.case_state == 2:
            self.case_state = 3 if keyword and text == "in" else 2
        elif self.case_state == 3:
            if keyword and text == "esac":
                self.cases -= 1
                self.case_state = 0
            else:
                self.case_state = 4
        elif self.case_state == 4:
            pass  # a pattern word, whatever it spells
        elif keyword and command and text == "case":
            self.cases += 1
            self.case_state = 1
        elif keyword and command and text == "esac" and self.cases:
            self.cases -= 1

    def _close_subst(self, i: int, nest: int = 0) -> int:
        """Index of the `)` closing a `$(` or `<(` whose body starts at `i`, or -1. The
        body is read by the lexer itself, so a `)` in a comment, a heredoc, a quote or a
        case pattern closes nothing, as in bash."""
        if self.depth + nest > _MAX_NESTING:
            raise _TooDeep
        sub = _BashLexer(self.s, self.depth + nest + 1)
        sub.i, sub.stop_paren, sub.collect = i, True, False
        sub.lex()
        return sub.stop_at

    def _close(self, i: int, open_ch: str, close_ch: str, nest: int = 0) -> int:
        """Index of the `close_ch` matching an `open_ch` just before `i`, or -1. A double
        quote holds its own substitutions, whose parentheses and quotes are their own."""
        if self.depth + nest > _MAX_NESTING:
            raise _TooDeep
        parens = open_ch == "("
        if parens and i - 1 in self.paren_match:
            return self.paren_match[i - 1]
        opened = [i - 1]
        s, n, depth = self.s, self.n, 1
        while i < n:
            ch = s[i]
            if ch == "\\":
                i += 2
                continue
            if s.startswith("$'", i):  # $'it\\'s': an escaped quote does not close it
                _text, i = self._ansi_c_fragment(i)
                if i < 0:
                    return -1
                continue
            if ch == "'":
                k = s.find("'", i + 1)
                if k < 0:
                    return -1
                i = k + 1
                continue
            if ch == '"':
                k = i + 1
                while k < n and s[k] != '"':
                    if s[k] == "\\":
                        k += 2
                    elif s.startswith("$(", k):
                        k = self._close_subst(k + 2, nest + 1)
                        if k < 0:
                            return -1
                        k += 1
                    elif s[k] == "`":
                        k = s.find("`", k + 1)
                        if k < 0:
                            return -1
                        k += 1
                    else:
                        k += 1
                if k >= n:
                    return -1
                i = k + 1
                continue
            if ch == open_ch:
                depth += 1
                if parens:
                    opened.append(i)
            elif ch == close_ch:
                depth -= 1
                if parens:
                    self.paren_match[opened.pop()] = i
                if depth == 0:
                    return i
            i += 1
        return -1

    def _subst(self, i: int, word: list[str], in_dquote: bool = False,
               tick_dquote: bool | None = None) -> int:
        """`$(`, `$((`, `${` or a backtick at `i`; returns the index after it, or -1.
        Outside double quotes a single quote inside `${...}` quotes; inside, it is text."""
        s = self.s
        if s.startswith("$((", i):
            k = self._close(i + 3, "(", ")")
            if k < 0 or k + 1 >= self.n or s[k + 1] != ")":
                k = self._close_subst(i + 2)
                if k < 0:
                    return -1
                self.subs.append(s[i + 2:k])
                word.append(s[i:k + 1])
                return k + 1
            word.append(s[i:k + 2])  # arithmetic: only the substitutions inside run
            self._expand(s[i + 3:k], self.depth + 1, dquotes=True)
            return k + 2
        if s.startswith("$(", i):
            k = self._close_subst(i + 2)
            if k < 0:
                return -1
            self.subs.append(s[i + 2:k])
            word.append(s[i:k + 1])
            return k + 1
        if s.startswith("${", i):
            k = self._close(i + 2, "{", "}")
            if k < 0:
                return -1
            word.append(s[i:k + 1])
            if not self.collect:
                return k + 1  # only its end was wanted; what runs inside is read elsewhere
            # ${x:-$(cmd)} runs cmd; ${x:-'$(cmd)'} does not, unless inside "...". A
            # pattern (${x#p} ${x%p} ${x/p/r} ${x^p} ${x,p}) quotes with '...' even there.
            inner = s[i + 2:k]
            j = 1 if inner[:1] in "!#" and inner[1:2] not in ("", "}") else 0  # ${!r} ${#a}
            start = j
            while j < len(inner) and (inner[j].isalnum() or inner[j] == "_"):
                j += 1
            if j == start and inner[j:j + 1] and inner[j] in "#?$!@*-0123456789":
                j += 1  # a special parameter (${#}, ${@}...) or ${#name}
            name_end = j
            if inner[j:j + 1] == "[":  # ${x[0]#p}: the subscript belongs to the name
                close = _BashLexer(inner, self.depth + 1)._close(j + 1, "[", "]")  # quote-aware
                j = close + 1 if close >= 0 else len(inner)
                # An indexed array's subscript is arithmetic and runs its substitutions
                # through any quotes; an associative one does not. The array's type is not
                # in the text, so the subscript is read as running them (a false refusal).
                self._expand(inner[name_end + 1:j - 1], self.depth + 1,
                                             dquotes=True)
            pattern = inner[j:j + 1] in ("#", "%", "/", "^", ",")
            # ${a:off:len} is arithmetic, which runs substitutions through any quotes.
            arithmetic = inner[j:j + 1] == ":" and inner[j + 1:j + 2] not in ("-", "=", "?", "+")
            self._expand(inner[j:], self.depth + 1, dquotes=arithmetic,
                                         single_quotes=not arithmetic and (pattern or not in_dquote))
            return k + 1
        k = i + 1  # a backtick
        while k < self.n and s[k] != "`":
            k += 2 if s[k] == "\\" else 1
        if k >= self.n:
            return -1
        # In backticks a backslash quotes only `$`, a backtick and itself (and `"` within
        # double quotes); bash removes it before the body is parsed, so `\\-n` is `-n`.
        dq = in_dquote if tick_dquote is None else tick_dquote  # a heredoc body is not "..."
        quoted = r'\\([\\$`"])' if dq else r"\\([\\$`])"
        self.subs.append(re.sub(quoted, r"\1", s[i + 1:k]))
        word.append(s[i:k + 1])
        return k + 1

    def _ansi_c_fragment(self, i: int) -> tuple[str, int]:
        """The decoded text of the `$'...'` starting at `i`, and the index after it; -1 if
        unterminated. A NUL ends the fragment's text, as in bash, where `$'a\\0b'c` is `ac`."""
        s, n, k = self.s, self.n, i + 2
        out: list[str] = []
        while k < n and s[k] != "'":
            if s[k] == "\\" and k + 1 < n:
                text, k = _ansi_c_escape(s, k)
                out.append(text)
            else:
                out.append(s[k])
                k += 1
        if k >= n:
            return "", -1
        return "".join(out).split("\0", 1)[0], k + 1

    def _heredoc_word(self) -> None:
        s, n = self.s, self.n
        strip = s.startswith("-", self.i)
        if strip:
            self.i += 1
        while self.i < n and s[self.i] in " \t":
            self.i += 1
        delim: list[str] = []
        quoted = False
        while self.i < n and s[self.i] not in " \t\n;&|()<>":
            ch = s[self.i]
            if s.startswith("$'", self.i):  # $'E\\x4fF' is EOF
                text, k = self._ansi_c_fragment(self.i)
                quoted = True
                delim.append(text)
                self.i = k if k >= 0 else n
                continue
            if s.startswith('$"', self.i):
                self.i += 1  # $"EOF": the quotes go, so does the dollar
                continue
            if s.startswith("\\\n", self.i):  # a line continuation is removed, not quoting
                self.i += 2
                continue
            if ch in "'\"\\":
                quoted = True
            if ch == "'":
                k = s.find(ch, self.i + 1)
                if k < 0:
                    k = n
                delim.append(s[self.i + 1:k])
                self.i = k + 1
            elif ch == '"':  # inside double quotes a backslash escapes " \\ $ and `
                k = self.i + 1
                while k < n and s[k] != '"':
                    if s.startswith("\\\n", k):  # a line continuation goes, here too
                        k += 2
                        continue
                    if s[k] == "\\" and k + 1 < n and s[k + 1] in '"\\$`':
                        k += 1
                    delim.append(s[k])
                    k += 1
                self.i = k + 1
            elif ch == "\\" and self.i + 1 < n:
                delim.append(s[self.i + 1])
                self.i += 2
            else:
                delim.append(ch)
                self.i += 1
        self.heredocs.append(["".join(delim), strip, quoted, False])
        self.cmd_heredocs.append(len(self.heredocs) - 1)

    def _end_command(self) -> None:
        """A simple command ended. When it is a shell reading its script from stdin
        (`bash <<EOF`, `bash -s -- a <<EOF`), EVERY heredoc it has is read as a script.
        Which one bash reads depends on descriptor plumbing (`3<<E <&3-`, `<&"3"`, a
        later `<file`) that this reader does not model, so it does not guess: a data
        heredoc that holds a commit is refused (one retry), a script is never missed."""
        if (self.cmd_heredocs or self.cmd_herestrings) and self._command_is_shell():
            for k in self.cmd_heredocs:
                self.heredocs[k][3] = True
            self.subs.extend(self.cmd_herestrings)  # a here-string is a one-line script
        self.cmd_heredocs = []
        self.cmd_herestrings = []

    def _command_is_shell(self) -> bool:
        words: list[str] = []
        for tok in reversed(self.toks):
            if isinstance(tok, _Op):
                break
            words.append(tok)
        words.reverse()
        words = _strip_wrappers(words)
        if not words or words[0].rsplit("/", 1)[-1] not in _SHELLS:
            return False
        args, from_stdin = words[1:], False
        k = 0
        while k < len(args) and args[k][:1] in "-+" and args[k] != "--":
            if args[k] in ("--rcfile", "--init-file", "-o", "-O", "+O", "+o"):
                k += 2
                continue
            flags = "" if args[k].startswith("--") else args[k][1:]
            if "c" in flags:
                return False  # bash -c: the script is an argument, stdin is data
            from_stdin = from_stdin or "s" in flags
            k += 1
        positional = args[k + 1:] if k < len(args) and args[k] == "--" else args[k:]
        return from_stdin or not positional  # else the first positional is a script file

    def _skip_heredoc_bodies(self) -> None:
        s = self.s
        for delim, strip, quoted, script in self.heredocs:
            body: list[str] = []
            while self.i < self.n:
                end = s.find("\n", self.i)
                line = s[self.i:end if end >= 0 else self.n]
                self.i = end + 1 if end >= 0 else self.n
                # An unquoted body joins a backslash-newline before the delimiter test.
                parts = [line]
                while not quoted and self.i < self.n and (len(parts[-1]) - len(parts[-1].rstrip("\\"))) % 2:
                    parts[-1] = parts[-1][:-1]
                    end = s.find("\n", self.i)
                    parts.append(s[self.i:end if end >= 0 else self.n])
                    self.i = end + 1 if end >= 0 else self.n
                line = "".join(parts) if len(parts) > 1 else line
                if (line.lstrip("\t") if strip else line) == delim:
                    break
                body.append(line)
            if script:  # fed to a shell: the body is a script and its commands run
                text = "\n".join(body)
                if not quoted:
                    # The outer shell expands an unquoted body first: `\\\\-n` reaches the
                    # inner shell as `\\-n`, and its substitutions run out here.
                    self._expand(text, self.depth + 1)
                    text = re.sub(r"\\([\\$`])", r"\1", text)
                self.subs.append(text)
            elif not quoted:  # an unquoted body expands as a whole: its substitutions run
                self._expand("\n".join(body), self.depth + 1)
        self.heredocs = []

    def lex(self) -> bool:
        """Tokenize the whole script; False when it ends inside a quote or substitution."""
        s, n = self.s, self.n
        word: list[str] = []
        inword = False

        def end_word():
            nonlocal inword
            if inword:
                if self.skip_operand:
                    self.skip_operand = False  # a redirection target is not an argument
                    if self.herestring_next:  # `bash <<< 'git commit'`: maybe a script
                        self.cmd_herestrings.append("".join(word))
                else:
                    text = "".join(word)
                    if self.word_quoted:
                        self._subscript(text)
                    if self.stop_paren:
                        self._case_word(text)
                    self.toks.append(text)
                word.clear()
                inword = False
                self.word_quoted = False

        def redirect(op_len: int):
            nonlocal inword
            fd = "".join(word)
            if inword and (fd.isdigit() or (fd.startswith("{") and fd.endswith("}"))):
                word.clear()  # `2>`, `{fd}>`: the number names a descriptor, not an argument
                inword = False
            else:
                fd = ""
            end_word()
            op = s[self.i:self.i + op_len]
            self.i += op_len
            self.herestring_next = op == "<<<"
            if op in ("<&", ">&"):
                j = self.i
                while j < n and s[j] in " \t":  # `<& 3` is `<&3`
                    j += 1
                k = j
                while k < n and s[k].isdigit():
                    k += 1
                if k < n and s[k] == "-":
                    k += 1
                if k > j:  # `>&2`, `<&3`, `<&3-`, `>&-`: a descriptor, no operand word
                    self.i = k
                    return
            self.skip_operand = True

        while self.i < n:
            ch = s[self.i]
            if ch == "\\":
                if self.i + 1 < n and s[self.i + 1] == "\n":
                    self.i += 2
                    continue
                word.append(s[self.i + 1] if self.i + 1 < n else ch)
                inword = self.word_quoted = True
                self.i += 2
                continue
            if ch == "'":
                k = s.find("'", self.i + 1)
                if k < 0:
                    return False
                word.append(s[self.i + 1:k])
                inword = self.word_quoted = True
                self.i = k + 1
                continue
            if s.startswith('$"', self.i):  # $"..." is a localized double quote
                self.i += 1
                continue
            if ch == "$" and s.startswith("$'", self.i):
                text, k = self._ansi_c_fragment(self.i)
                if k < 0:
                    return False
                word.append(text)
                inword = self.word_quoted = True
                self.i = k
                continue
            if ch == '"':
                k = self.i + 1
                while k < n and s[k] != '"':
                    c = s[k]
                    if c == "\\" and k + 1 < n:
                        nx = s[k + 1]
                        word.append("" if nx == "\n" else nx if nx in '$`"\\' else c + nx)
                        k += 2
                    elif c == "`" or (c == "$" and k + 1 < n and s[k + 1] in "({"):
                        k = self._subst(k, word, in_dquote=True)
                        if k < 0:
                            return False
                    else:
                        word.append(c)
                        k += 1
                if k >= n:
                    return False
                inword = self.word_quoted = True
                self.i = k + 1
                continue
            if ch == "`" or (ch == "$" and self.i + 1 < n and s[self.i + 1] in "({"):
                k = self._subst(self.i, word)
                if k < 0:
                    return False
                inword = True
                self.i = k
                continue
            if ch == "#" and not inword and not self.pattern:
                k = s.find("\n", self.i)
                self.i = k if k >= 0 else n
                continue
            if ch in "<>" and self.i + 1 < n and s[self.i + 1] == "(":
                k = self._close_subst(self.i + 2)  # process substitution
                if k < 0:
                    return False
                self.subs.append(s[self.i + 2:k])
                word.append(s[self.i:k + 1])
                inword = True
                self.i = k + 1
                continue
            if s.startswith("<<<", self.i):
                redirect(3)
                continue
            if s.startswith("<<", self.i):
                fd = "".join(word)
                if word and (fd.isdigit() or (fd.startswith("{") and fd.endswith("}"))):
                    word.clear()  # `0<<EOF`, `{fd}<<EOF`: a descriptor, not a word
                    inword = False
                end_word()
                self.i += 2
                self._heredoc_word()
                continue
            if ch in "<>" or s.startswith("&>", self.i):
                two = s[self.i:self.i + 2]
                if s.startswith("&>>", self.i):
                    redirect(3)
                elif two in (">>", ">&", ">|", "<>", "<&", "&>"):
                    redirect(2)
                else:
                    redirect(1)
                continue
            if ch in " \t":
                end_word()
                self.i += 1
                continue
            if ch == "\n":
                end_word()
                self.skip_operand = False
                self._end_command()
                self.toks.append(_SEP)
                self.i += 1
                self._skip_heredoc_bodies()
                continue
            if ch == "(" and inword and word and word[-1][-1:] in ("@", "!", "*", "+", "?"):
                k = self._close(self.i + 1, "(", ")")  # extglob `@(y|z)`: part of the word
                if k < 0:
                    return False
                word.append(s[self.i:k + 1])
                # Its patterns are words, so the lexer reads them: their substitutions
                # (`$(...)`, `<(...)`) run, their quotes quote, and their words are no command.
                # A close-finding lexer has its position from `_close` and keeps no
                # subs, so it does not read them: reading them too doubled the work
                # at every nesting level.
                if self.collect:
                    group = _BashLexer(s[self.i + 1:k], self.depth + 1)
                    group.pattern = True
                    if not group.lex():
                        return False
                    self.subs.extend(group.subs)
                self.i = k + 1
                continue
            if ch == "(" and not inword and not self.compound and s.startswith("((", self.i):
                # `(( x = 1 << 2 ))`, `for (( ...; ...; ))`: an arithmetic command, whose
                # `<<` is a shift, not a heredoc. Only its substitutions run. Without a
                # closing `))` it is two subshells, as in bash. With no `)` closing it at
                # all the script is unfinished: stop, and the caller asks bash about the
                # rest (rescanning to the end for every later `((` was quadratic).
                k = self._close(self.i + 2, "(", ")")
                if k < 0:
                    return False
                if k >= 0 and k + 1 < n and s[k + 1] == ")":
                    self._expand(s[self.i + 2:k], self.depth + 1, dquotes=True)
                    self.toks.append(_SEP)
                    self.i = k + 2
                    continue
            if ch in ";&|()":
                end_word()
                if ch == "(" and self.toks and not isinstance(self.toks[-1], _Op) and \
                        _COMPOUND_ASSIGN_RE.fullmatch(self.toks[-1]):
                    self.compound = True
                elif ch == ")":
                    self.compound = False
                if self.stop_paren and self.case_state in (3, 4) and ch in "()|":
                    # `)` ends the pattern list and its commands follow; `(` opens it,
                    # optionally; `|` separates patterns.
                    self.case_state = 0 if ch == ")" else 4
                    self.toks.append(_SEP)
                    self.i += 1
                    continue
                if self.stop_paren and ch == ";" and self.cases and s.startswith(";;", self.i):
                    self.case_state = 3  # `;;`, `;;&`: a pattern is next
                elif self.stop_paren and ch == ";" and self.cases and s.startswith(";&", self.i):
                    self.case_state = 3
                if self.stop_paren and ch in "()":
                    if ch == "(":
                        self.parens += 1
                    elif self.parens:
                        self.parens -= 1
                    else:  # the substitution's own `)`
                        self.stop_at = self.i
                        self.skip_operand = False
                        self._end_command()
                        return True
                self.skip_operand = False
                self._end_command()
                two = s[self.i:self.i + 2]
                if two in ("&&", "||", "|&"):
                    self.toks.append(_Op(two))
                    self.i += 2
                else:
                    self.toks.append(_SEP if ch in ";()" else _Op(ch))
                    self.i += 1
                continue
            word.append(ch)
            inword = True
            self.i += 1
        end_word()
        self._end_command()
        return True


# Words that open or close a compound command; the command after one starts a fresh
# argv (`if git commit -n -m x; then`, `{ git commit -n; }`).
_RESERVED_LEADERS = frozenset(("if", "then", "else", "elif", "fi", "do", "done", "while",
                               "until", "!", "{", "}", "time"))


def _expansions(text: str, depth: int = 0, single_quotes: bool = False,
                dquotes: bool = False) -> list[str]:
    """The commands inside the substitutions of `text` read as plain expanding text
    (an unquoted heredoc body, the inside of `${...}` or `$((...))`). With
    `single_quotes`, '...' and $'...' quote, as in an unquoted `${x:-'...'}`."""
    lx = _BashLexer(text, depth)
    scratch: list[str] = []
    dquote = False  # a "..." opened inside: single quotes are text there
    i = 0
    while i < lx.n:
        ch = text[i]
        if ch == "\\":
            i += 2
            continue
        if (single_quotes or dquotes) and ch == '"':  # not in a heredoc body: text there
            dquote = not dquote
            i += 1
            continue
        if single_quotes and not dquote and text.startswith("$'", i):
            _text, i = lx._ansi_c_fragment(i)
            if i < 0:
                break
            continue
        if single_quotes and not dquote and ch == "'":
            k = text.find("'", i + 1)
            if k < 0:
                break
            i = k + 1
            continue
        if ch == "`" or (ch == "$" and i + 1 < lx.n and text[i + 1] in "({"):
            # A heredoc body or a "..." is double-quote context for a nested ${...}.
            k = lx._subst(i, scratch, in_dquote=dquote or not single_quotes, tick_dquote=dquote)
            if k < 0:
                lx.subs.append(text[i:])  # unterminated: read the rest as a command
                break
            i = k
            continue
        i += 1
    return lx.subs


def _script_tokens(cmd: str, _depth: int = 0) -> tuple[list[str], str, int]:
    """(tokens, unread, unread_line) of a whole script, its commands separated by `;`.
    NOTHING IS REMOVED that bash would run: heredoc bodies and comments go, redirection
    targets leave the argv, substitutions are read as commands.

    `unread` is the text from where the script stopped parsing (an unterminated quote or
    substitution), and `unread_line` its 1-based line in `cmd` (0 when it came from inside
    a substitution). bash runs the complete lines before it; the caller decides about the
    rest.
    """
    lx = _BashLexer(cmd, _depth)
    complete = lx.lex()
    toks = lx.toks
    unread = "" if complete else cmd[lx.i:]
    unread_line = 0 if complete else cmd.count("\n", 0, lx.i) + 1
    # nesting is bounded by _BashLexer, which raises _TooDeep. A body collected twice
    # (its substitution and a subscript holding it) is read once.
    for sub in dict.fromkeys(lx.subs):
        sub_toks, sub_unread, _line = _sub_script_tokens(sub, _depth + 1)
        toks.append(_SEP)
        toks.extend(sub_toks)
        if not unread and sub_unread:
            unread, unread_line = sub_unread, 0
    return toks, unread, unread_line


@functools.lru_cache(maxsize=512)
def _sub_script_tokens(cmd: str, depth: int) -> tuple[tuple, str, int]:
    """_script_tokens of a substitution body, read once however often it is collected.
    A body not read before is charged to the reading's re-read budget: nested
    `eval "$(...)"` restarts the depth and misses this cache at every level (11 s at
    24). Past the budget the whole command is judged unread."""
    global _REREAD_LEFT
    _REREAD_LEFT -= len(cmd)
    if _REREAD_LEFT < 0:
        raise _TooDeep
    toks, unread, line = _script_tokens(cmd, depth)
    return tuple(toks), unread, line


_BASH_ERROR_LINE = re.compile(r"line (\d+):")


# `bash -n` runs one top-level reading may spend: each is a process (~2 ms), and 6000
# distinct malformed `bash -c` bodies naming a commit took 10.5 s. Past it the rest is
# read conservatively. A real command needs one or two.
_SYNTAX_CHECK_BUDGET = 64
_SYNTAX_CHECKS_LEFT = _SYNTAX_CHECK_BUDGET


def _bash_rejects_from(cmd: str, line: int) -> bool:
    """True when bash refuses to parse `cmd` at or after `line`, so nothing from there runs.

    `bash -n` reads without executing: no command, substitution or expansion runs. bash
    runs a script one complete command at a time, so the lines before a syntax error do
    run (`git commit -n -m x` then a stray quote commits); only the unfinished command and
    what follows it do not. Anything that stops the check answers False, and the caller
    then reads the unfinished text conservatively.
    """
    global _SYNTAX_CHECKS_LEFT
    if line < 1:
        return False
    if _SYNTAX_CHECKS_LEFT <= 0:
        return False  # fail-direction: past the budget the text is read conservatively
    _SYNTAX_CHECKS_LEFT -= 1
    try:
        done = subprocess.run(["bash", "-n", "-c", cmd], capture_output=True, text=True,
                              timeout=5)  # deadline: 5 s, a parse only
    except (OSError, subprocess.SubprocessError):
        return False  # fail-direction: False keeps the conservative reading
    if done.returncode == 0:
        return False
    m = _BASH_ERROR_LINE.search(done.stderr or "")
    return bool(m) and int(m.group(1)) >= line


_PLAIN_WORD = re.compile(r"[A-Za-z0-9_.,/=+-]+")


def _unread_commit(text: str) -> tuple[bool, bool]:
    """(is_commit, has_no_verify) for text the gate could not read: the rest of a script
    the lexer could not finish though bash would, or a command nested too deep.

    Unread text can hold a no-verify flag in any encoding (`$'\\x2dn'`), so a `git` word
    followed later by a `commit` word is a commit AND is refused. A false refusal costs
    one retry; a hidden `-n` disables the gate.
    """
    # Quotes and backslashes join a word in bash (`c'ommit'`, `gi\\t`): drop them first.
    words = _PLAIN_WORD.findall(re.sub(r"[\'\"\\\\]", "", text))
    words = [w.rsplit("/", 1)[-1] for w in words]  # /usr/bin/git is git
    if "git" not in words or "commit" not in words[words.index("git"):]:
        return (False, False)
    return (True, True)


_COMMIT_MEMO: dict | None = None  # one top-level reading's answers, by script text


def _bash_is_git_commit(cmd: str) -> tuple[bool, bool]:
    """_read_git_commit, each script read once per top-level call. A shell's script
    (`eval "$(...)"`, `bash -c "$(...)"`) is read again where its command is judged,
    and nested ones re-read every deeper level: 2x the work per level, 2.5 s at twelve."""
    global _COMMIT_MEMO, _REREAD_LEFT, _SYNTAX_CHECKS_LEFT
    outer = _COMMIT_MEMO is None
    if outer:
        _COMMIT_MEMO = {}
        _REREAD_LEFT = _REREAD_BUDGET
        _SYNTAX_CHECKS_LEFT = _SYNTAX_CHECK_BUDGET
    try:
        if cmd not in _COMMIT_MEMO:
            _COMMIT_MEMO[cmd] = _read_git_commit(cmd)
        return _COMMIT_MEMO[cmd]
    finally:
        if outer:
            _COMMIT_MEMO = None


def _read_git_commit(cmd: str) -> tuple[bool, bool]:
    """Returns (is_git_commit, has_no_verify) for any command `cmd` runs:
    each simple command of the script as bash reads it (_BashLexer), the
    commands inside its substitutions, and an inner `bash -c "..."` via
    _segment_is_git_commit.
    """
    if not isinstance(cmd, str) or not cmd.strip():
        return (False, False)
    try:
        toks, unread, unread_line = _script_tokens(cmd)
    except (_TooDeep, RecursionError):
        return _unread_commit(cmd)  # too deep to read: refuse a possible commit, never crash
    saw_commit = saw_no_verify = False
    # Unread text with no possible commit in it changes nothing, so bash -n is asked only
    # when its answer could (one spawn per malformed body: 5500 took 11 s).
    if unread and _unread_commit(cmd)[0] and not _bash_rejects_from(cmd, unread_line):
        saw_commit, saw_no_verify = _unread_commit(cmd)  # the whole command: its prefix too
    if saw_no_verify:
        return (True, True)
    for seg in _commands(toks):
        lead = 0  # an index, not `seg = seg[1:]`: 160000 leading `!` copied the rest each time
        while lead < len(seg) and seg[lead] in _RESERVED_LEADERS:
            lead += 1
        seg = seg[lead:]
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


# The passes of the reviewer pin (.conclave/reviewers.toml). A section with a
# receipt for any of them was reviewed under the pin and is judged from those
# receipts; a section with none is judged by its older stamps as before, so a
# section already under review finishes on the rules it started with.
#
# A receipt is the line scripts/codex-dispatch.sh appends to the reviews
# manifest once it has checked a pass: the candidate it identified BEFORE the
# reviewer ran, whether every lens gave a verdict, and the review's worst
# level. The recorder's stamp file is not consulted for a pass: it is written
# when a dispatch STARTS, which for the run's background broker is before
# there is anything to judge, and it keeps one entry per TODO across sections.
_PASS_KINDS = ("plan", "round", "performance", "completion", "reconciliation")
_PASS_MANIFEST = ".claude/overnight/reviews/manifest.jsonl"
_BLOCKING_FALLBACK = ("P1", "P2")


# What may change after a section's last pass without a new pass: the TODO
# record the stamp itself edits, and files a commit regenerates. Everything
# else is content the pass has not read. Wider than _DISPATCH_CONTENT_PATHS on
# purpose: that set is kernel-shaped, and a tooling section's whole candidate
# lives under .claude/, scripts/ and .githooks/.
#   - Markdown under todo/: the record (stamps, ticked items, the plan file).
#     Anything else placed there is content.
#   - docs/test-coverage/ and COUNT.md: written whole by the test run and the
#     post-commit hook.
#   - README.md, but only its `<!-- project:key -->` regions, which the
#     post-commit hook re-syncs; a hand edit anywhere else is content
#     (_readme_only_regenerated).
_FINAL_PASS_GENERATED_DIRS = ("docs/test-coverage/",)
_FINAL_PASS_GENERATED_FILES = frozenset({"COUNT.md"})
_README_REGION_RE = re.compile(r"<!-- project:([a-z_]+) -->.*?<!-- /project -->", re.S)
_SECTION_HEADING_RE = re.compile(r"^## (\d+)\.")
_HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def _readme_only_regenerated(root: Path, old_spec: str, new_spec: str) -> bool:
    """Whether README.md differs between two blobs only inside its generated regions."""
    def stripped(spec: str) -> str | None:
        try:
            text = subprocess.check_output(["git", "show", spec], cwd=str(root), text=True, timeout=10,
                                           stderr=subprocess.DEVNULL)
        except Exception:
            return None
        return _README_REGION_RE.sub(lambda m: f"<!-- project:{m.group(1)} --><!-- /project -->", text)
    a, b = stripped(old_spec), stripped(new_spec)
    return a is not None and a == b


def _unread_by_final_pass(root: Path, paths: list[str], old_spec: str, new_spec: str) -> list[str]:
    """The paths among `paths` that hold content a pass reads.

    `old_spec` and `new_spec` are the git prefixes of the two sides (`<sha>:`
    or `:` for the index), used to read README.md on each.
    """
    unread = []
    for p in paths:
        if not p or p in _FINAL_PASS_GENERATED_FILES or p.startswith(_FINAL_PASS_GENERATED_DIRS):
            continue
        if p.startswith("todo/") and p.endswith(".md"):
            continue
        if p == "README.md" and _readme_only_regenerated(root, old_spec + p, new_spec + p):
            continue
        unread.append(p)
    return unread


def _git_lines(root: Path, *args: str) -> list[str] | None:
    try:
        out = subprocess.check_output(["git", *args], cwd=str(root), text=True, timeout=10,
                                      stderr=subprocess.DEVNULL)
    except Exception:
        return None
    return out.splitlines()


def _stamped_sections(root: Path, todo: str) -> set[str] | None:
    """The section numbers whose stamp the staged diff of `todo` adds; None when they cannot be read.

    A stamp line belongs to the nearest `## N.` heading above it in the STAGED file.
    """
    diff = _git_lines(root, "diff", "--cached", "-U0", "--", todo)
    staged = _git_lines(root, "show", f":{todo}")
    if diff is None or staged is None:
        return None
    sections: set[str] = set()
    line_no = 0
    for line in diff:
        m = _HUNK_RE.match(line)
        if m:
            line_no = int(m.group(1))
            continue
        if not line.startswith("+") or line.startswith("+++"):
            continue
        if _STAMP_ADDED_RE.match(line):
            found = None
            for i in range(min(line_no, len(staged)) - 1, -1, -1):
                h = _SECTION_HEADING_RE.match(staged[i])
                if h:
                    found = h.group(1)
                    break
            if found is None:
                return None
            sections.add(found)
        line_no += 1
    return sections or None


def _blocking_levels(root: Path) -> tuple[str, ...]:
    """The levels that hold a stamp, from the reviewer pin; P1 and P2 when the pin cannot be read."""
    scripts = str(root / "scripts")
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    try:
        import review_pin
        return tuple(review_pin.load_pin(root / ".conclave" / "reviewers.toml")["blocking"])
    except Exception:
        return _BLOCKING_FALLBACK   # a broken pin must not open the gate


def _pass_receipts(root: Path, todo: str, section: str | None) -> tuple[dict, bool]:
    """(newest receipt per pass kind, readable) for one TODO, and one section when given.

    The manifest is read newest first. A line that cannot be read may be a
    newer, blocking receipt cut short while it was written; reading past it
    would revive an older clean one. So when such a line names this TODO, or is
    cut too short to say which TODO it was for, the scan stops there and
    `readable` is False: a kind not found before it has no receipt that can be
    trusted. An unreadable line that names another TODO is not this one's.
    Each value is (receipt, age), a lower age being a newer line.
    """
    found: dict = {}
    try:
        lines = (root / _PASS_MANIFEST).read_text().splitlines()
    except FileNotFoundError:
        return (found, True)      # no pass has ever been dispatched here
    except (OSError, UnicodeDecodeError):
        return (found, False)     # it exists and cannot be read: nothing may be assumed absent
    age = 0
    for line in reversed(lines):
        if not line.strip():
            continue
        age += 1
        try:
            e = json.loads(line)
        except ValueError:
            if '"todo": "' not in line or f'"todo": "{todo}"' in line:
                return (found, False)
            continue
        if not isinstance(e, dict) or e.get("todo") != todo or e.get("kind") not in _PASS_KINDS:
            continue
        if section is not None and str(e.get("section", "")) != section:
            continue
        found.setdefault(e["kind"], (e, age))
    return (found, True)


def _pass_cap(root: Path) -> int | None:
    """The reviewer pin's pass_cap, or None when the pin cannot be read (no waiver then)."""
    scripts = str(root / "scripts")
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    try:
        import review_pin
        return int(review_pin.load_pin(root / ".conclave" / "reviewers.toml")["pass_cap"])
    except Exception:
        return None


def _spent_passes(root: Path, todo: str, section: str) -> int:
    """How many checked, complete passes this section has had: its spend against pass_cap."""
    try:
        lines = (root / _PASS_MANIFEST).read_text().splitlines()
    except (OSError, UnicodeDecodeError):
        return 0
    spent = 0
    for line in lines:
        try:
            e = json.loads(line)
        except ValueError:
            continue
        if isinstance(e, dict) and e.get("todo") == todo and str(e.get("section", "")) == section \
                and e.get("kind") in _PASS_KINDS and _receipt_complete(e):
            spent += 1
    return spent


def _receipt_complete(receipt: dict) -> bool:
    return receipt.get("route") == "codex-dispatch.sh" and receipt.get("pass_checked") is True \
        and receipt.get("rc") == 0 and isinstance(receipt.get("head"), str) and len(receipt["head"]) >= 7


def _pass_covers(root: Path, kind: str, receipt: dict | None, current_head: str,
                 blocking: tuple[str, ...]) -> tuple[bool, str]:
    """Whether a pass read the candidate being stamped and ended below a blocking level.

    Its receipt must say the pass was checked and complete. The candidate it
    names must be HEAD, or an ancestor with nothing changed since that a pass
    reads (_unread_by_final_pass): a fix made after it is exactly what it has
    not seen. And its worst finding must not be one that holds the stamp.
    """
    if receipt is None:
        return (False, f"missing: {kind}")
    if not _receipt_complete(receipt):
        return (False, f"{kind} has no checked, complete receipt (still running, incomplete, or not "
                       f"dispatched with scripts/codex-dispatch.sh)")
    head = receipt["head"]
    if not current_head:
        return (False, f"{kind}: HEAD could not be read")
    if head != current_head:
        if not _git_is_ancestor(root, head, current_head):
            return (False, f"{kind} was dispatched at {head[:10]}, which is not in this history (cross-branch)")
        # --no-renames: a script moved into todo/ must show as removed from where it ran.
        changed = _git_lines(root, "diff", "--no-renames", "--name-only", head, current_head)
        if changed is None:
            return (False, f"{kind}: could not list what changed since {head[:10]}")
        unread = _unread_by_final_pass(root, changed, f"{head}:", f"{current_head}:")
        if unread:
            more = f" (+{len(unread) - 3} more)" if len(unread) > 3 else ""
            return (False, f"{kind} is not the last word on this candidate: changed since it ran: "
                           f"{unread[:3]}{more}")
    level = receipt.get("highest")
    if level not in ("P1", "P2", "P3", "none"):
        return (False, f"{kind} receipt at {head[:10]} records no HIGHEST level")
    if level in blocking:
        return (False, f"{kind} ended at {level}: fix its findings, commit, and dispatch it again")
    return (True, "")


def _three_pass_issues(root: Path, section: str, receipts: dict, readable: bool,
                       current_head: str, capped: bool = False) -> list[str]:
    """What a section reviewed under the reviewer pin still owes before its stamp, as message parts.

    - plan must have been checked, on this history. It precedes the code, so
      it cannot be required to match the final source.
    - round, performance and the last pass (the newer of completion and
      reconciliation) must each cover the candidate and end below a blocking
      level (_pass_covers). The round and the performance pass are dispatched
      again after their findings are fixed, so the receipt that counts is the
      one for the fixed candidate.
    - Nothing a pass reads may ride along in the stamp commit.
    - `capped`: the section has spent pass_cap passes, which ends its round loop
      (docs/specs/2026-11-11-three-pass-review-design.md). The round is then not
      required to be clean; the performance pass and the last pass still must
      read the candidate and end below a blocking level, so every fix after the
      last round is read by a pass.
    """
    parts: list[str] = []
    blocking = _blocking_levels(root)
    # Receipts found before an unreadable line are newer than it and stand. Only
    # a pass that would have to be looked for PAST that line is affected, so
    # dispatching it again restores the section.
    needed = ("plan", "round", "performance", "completion")
    if not readable and any(k not in receipts for k in needed):
        parts.append(f"{_PASS_MANIFEST} cannot be read past a line that may be this TODO's; a pass not found "
                     f"before it is not trusted (dispatch that pass again, or repair the line)")

    plan = receipts.get("plan", (None, 0))[0]
    if plan is None:
        parts.append("missing: plan")
    elif not _receipt_complete(plan):
        parts.append("plan has no checked, complete receipt")
    elif not current_head or not _git_is_ancestor(root, plan["head"], current_head):
        parts.append(f"plan was dispatched at {plan['head'][:10]}, which is not in this history (cross-branch)")

    completion, c_age = receipts.get("completion", (None, 0))
    reconciliation, r_age = receipts.get("reconciliation", (None, 0))
    newer_reconciliation = completion is not None and reconciliation is not None and r_age < c_age
    final_kind, final = ("reconciliation", reconciliation) if newer_reconciliation else ("completion", completion)
    for kind, receipt in (("round", receipts.get("round", (None, 0))[0]),
                          ("performance", receipts.get("performance", (None, 0))[0]),
                          (final_kind, final)):
        if kind == "round" and capped and receipt is not None:
            continue
        ok, why = _pass_covers(root, kind, receipt, current_head, blocking)
        if not ok:
            parts.append(why)

    staged = _git_lines(root, "diff", "--cached", "--no-renames", "--name-only")
    if staged is None:
        parts.append("the staged files could not be listed")
    else:
        riding = _unread_by_final_pass(root, staged, "HEAD:", ":")
        if riding:
            more = f" (+{len(riding) - 3} more)" if len(riding) > 3 else ""
            parts.append(f"staged with the stamp but read by no pass: {riding[:3]}{more}; "
                         f"commit them first, then dispatch the passes again")
    return parts


def _four_dispatch_evidence(
    root: Path, stamped_todos: list[str]
) -> tuple[bool, str]:
    """Review evidence for every TODO whose staged diff adds a stamp.

    A section with a pass receipt is judged from its receipts
    (_three_pass_issues). Every other one is judged by its older stamps
    (_legacy_dispatch_evidence), exactly as before the reviewer pin.
    """
    if not stamped_todos:
        return (True, "")
    current_head = ""
    try:
        current_head = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=str(root),
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        pass
    issues: list[str] = []
    legacy: list[str] = []
    for todo in stamped_todos:
        any_receipts, any_readable = _pass_receipts(root, todo, None)
        if not any_receipts and any_readable:
            legacy.append(todo)
            continue
        sections = _stamped_sections(root, todo)
        if sections is None:
            owed = ["the section being stamped could not be read from the staged TODO"]
        elif len(sections) != 1:
            owed = [f"one commit stamps one section; this one stamps {sorted(sections)}"]
        else:
            section = next(iter(sections))
            receipts, readable = _pass_receipts(root, todo, section)
            if not receipts and readable:
                # This TODO has passes, but none for this section: it started under the older kinds.
                legacy.append(todo)
                continue
            cap = _pass_cap(root)
            capped = cap is not None and _spent_passes(root, todo, section) >= cap
            owed = _three_pass_issues(root, section, receipts, readable, current_head, capped)
        if owed:
            issues.append(
                f"{todo}: three-pass review incomplete -- {'; '.join(owed)}. "
                f"Dispatch the pass with scripts/codex-dispatch.sh and "
                f"`[review-kind: plan|round|performance|completion|reconciliation]`"
            )
    if issues:
        joined = " | ".join(issues[:5])
        more = f" (+{len(issues)-5} more)" if len(issues) > 5 else ""
        return (False, f"the stamp is refused: {joined}{more}")
    return _legacy_dispatch_evidence(root, legacy)


def _legacy_dispatch_evidence(
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



# Where the harness leaves the identity of the session issuing a Bash call, so
# the --git-hook-mode child (which gets no stdin payload) can tell whether an
# in-flight skill entry belongs to the committer or to someone else.
_COMMITTING_SESSION_REL = ".claude/state/committing-session.json"
# The marker is only trusted this long. A raw `git commit` typed in a terminal
# with no harness session must NOT be attributed to whoever last used the tool,
# so a stale marker is ignored and the gate falls back to its previous
# behaviour (assume the entry is live).
_COMMITTING_SESSION_FRESH_SEC = 300


def _record_committing_session(session_id) -> None:
    """Best-effort note of the session making this Bash call. Never raises."""
    if not isinstance(session_id, str) or not session_id:
        return
    try:
        root = _repo_root()
        if root is None:
            return
        p = root / _COMMITTING_SESSION_REL
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_suffix(p.suffix + ".tmp")
        tmp.write_text(json.dumps({"session_id": session_id,
                                   "ts": time.time()}), encoding="utf-8")
        tmp.replace(p)
    except Exception:
        pass


def _entry_is_foreign(root: Path, entry: dict) -> bool:
    """True only with POSITIVE evidence the entry belongs to another session.

    Requires all of: a marker file, a fresh timestamp, a session id on the
    entry, and a genuine mismatch. Anything missing or stale returns False, so
    this can only ever RELAX the gate on evidence and never tightens it or
    guesses. A human committing from a bare terminal has no fresh marker and is
    therefore treated exactly as before.
    """
    try:
        owner = entry.get("session_id")
        if not isinstance(owner, str) or not owner:
            return False
        raw = (root / _COMMITTING_SESSION_REL).read_text(encoding="utf-8")
        cur = json.loads(raw)
        sid = cur.get("session_id")
        ts = cur.get("ts")
        if not isinstance(sid, str) or not sid:
            return False
        if not isinstance(ts, (int, float)):
            return False
        if (time.time() - float(ts)) > _COMMITTING_SESSION_FRESH_SEC:
            return False                    # stale marker -> no evidence
        return sid != owner
    except Exception:
        return False


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


def _skill_entry_finished(skill: str, entry: dict) -> bool:
    """True when a skill-progress entry has already reached EVERY required
    terminal step, i.e. the skill ran to completion and is not in flight.

    WHY. Nothing clears a `review-todo-section` entry on NORMAL completion --
    `_skill_entry_stale` only ages one out after 6h and `compaction_orphaned`
    only covers entries a PreCompact saw. So a review that finished and stamped
    stays "active" for hours, and the `not_section` branch then reads the NEXT
    section's very first commit as that finished review's fix loop. MEASURED
    2026-08-18 on TODO-13 section 22: a first commit touching only
    `src/boot/uefi/efi.h` (+~90 lines, so the LOC arm fired) was BLOCKED for
    missing re-adversarial evidence belonging to the previous session's
    section-28 review. The obvious way out is to redispatch an adversarial
    round over an already-approved diff, which is exactly the churn
    `review_convergence.py` exists to stop.

    Terminal step 17 IS the review's own commit, so an entry carrying every
    required step has, by the gate's own definition of done, finished. A fix
    loop runs BEFORE that commit, never after it, so this cannot excuse a
    genuinely mid-review fix-loop commit -- an entry missing ANY required step
    is still live and the gate still fires. Fails CLOSED (returns False, i.e.
    "still active") on any parse problem.
    """
    try:
        from skill_step_map import REQUIRED_TERMINAL_STEPS
        required = set(REQUIRED_TERMINAL_STEPS.get(skill) or [])
        if not required:
            return False
        observed = entry.get("steps_observed")
        if not isinstance(observed, list):
            return False
        seen = {o.get("n") for o in observed if isinstance(o, dict)}
        return required.issubset(seen)
    except Exception:
        return False


_ARGS_SECTION_RE = re.compile(r"(?:\bsection\b[\s_:-]*|§\s*)(\d+)", re.IGNORECASE)


def _active_section_todo(root: Path) -> str:
    """The TODO path of the in-flight implement/review-todo-section skill, or ""."""
    return _active_section(root)[0]


def _active_section(root: Path) -> tuple[str, str]:
    """(TODO path, section number) of the in-flight implement/review-todo-section skill; "" for what is unknown.

    Reads skill-progress.json, preferring the structured `todo_path` recorded by
    skill_step_observer at skill-start (a single authoritative regex run) over a
    loose parse of `args`. Checks review-todo-section first (the section-ship
    context), then implement-todo-section (a fix-loop commit before review).
    The section is the entry's own `section`, else the one its `args` name."""
    state_path = root / ".claude" / "state" / "skill-progress.json"
    if not state_path.exists():
        return ("", "")
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
    except Exception:
        return ("", "")
    if not isinstance(state, dict):
        return ("", "")
    for skill in ("review-todo-section", "implement-todo-section"):
        entry = state.get(skill)
        if not isinstance(entry, dict) or entry.get("compaction_orphaned") is True:
            continue
        if _skill_entry_stale(entry) or _entry_is_foreign(root, entry):
            continue
        if _skill_entry_finished(skill, entry):
            continue
        args = entry.get("args", "")
        args = args if isinstance(args, str) else ""
        section = entry.get("section")
        section = str(section).lstrip("§").strip() if isinstance(section, (str, int)) and not isinstance(section, bool) else ""
        named = {m.group(1) for m in _ARGS_SECTION_RE.finditer(args)}
        if section.isdigit():
            named.add(section)
        # Two identifiers that disagree identify nothing: the caller keeps its hold.
        section = named.pop() if len(named) == 1 else ""
        todo_path = entry.get("todo_path")
        if isinstance(todo_path, str) and todo_path.endswith(".md") \
           and "todo/" in todo_path.replace("\\", "/"):
            return (todo_path, section)
        if args:
            m = re.search(r"todo/[\w./-]+TODO-\d[\w./-]*\.md", args)
            if m:
                return (m.group(0), section)
    return ("", "")


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

    active_todo, active_section = _active_section(root)
    if active_section and active_todo == todo and any(
            _receipt_complete(receipt)
            for receipt, _age in _pass_receipts(root, todo, active_section)[0].values()):
        # A section reviewed under the reviewer pin has no review between a
        # fix and its commit: the fix is committed and the passes then read
        # that candidate. The stamp commit is where they are required to cover
        # the final source (_three_pass_issues), so a fix commit is not held
        # here. Only for the section in flight and a checked, complete receipt
        # of its own: another section's passes say nothing about this one, a
        # broker's registration of a job that was then refused is not a pass,
        # and a section that cannot be identified keeps the hold.
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
    # Record WHO is issuing this Bash call, for the --git-hook-mode child.
    # That mode receives no stdin payload and therefore cannot tell whose
    # session is committing, which is how a finished run's in-flight skill
    # entry came to gate an unrelated interactive commit (2026-07-28). This
    # hook fires PreToolUse on every Bash call, including the `git commit`
    # that spawns pre-commit, so it is the natural place to leave that
    # identity behind. Best-effort: a failure here must never block a tool.
    _record_committing_session(payload.get("session_id"))
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


_SHELL_META = frozenset("\n;&|$`()<>\\#*?[]{}~!")


def _commit_target_root(cmd: str) -> Path | None:
    """The work tree a bare `git -C <dir> commit ...` call commits to.

    Live 2026-11-11: a commit in the repair worktree beside a run was refused on the
    PRIMARY tree's staged section, because the gate always read the index of its own
    directory. Only this one shape is read, as a whole command with no shell syntax
    anywhere in it; every other command keeps the gate's own directory, so a shape
    this does not recognise is judged exactly as before.
    """
    if not isinstance(cmd, str) or _SHELL_META & set(cmd):
        return None
    try:
        toks = shlex.split(cmd, posix=True, comments=False)
    except ValueError:
        return None
    if len(toks) < 4 or toks[:2] != ["git", "-C"] or toks[3] != "commit":
        return None
    try:
        out = subprocess.check_output(
            ["git", "-C", toks[2], "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return None
    return Path(out) if out else None


COMMIT_LANDED_DIR = "claude-commit-landed"
_SESSION_ID_RE = re.compile(r"[A-Za-z0-9-]{8,64}")


def commit_landed_roots(session_id: str, consumer: str) -> list[str]:
    """The work trees this session committed to since `consumer` last asked.

    For the post-commit hooks. Live 2026-11-11: a commit made in the repair
    worktree ran the kernel suite in the PRIMARY tree, because the hooks tested
    their own directory whenever the command looked like a commit. It rebuilt
    build/ under a live run's smoke matrix twice.

    Nothing is inferred from the command text or from a timestamp. git's own
    post-commit hook (.githooks/post-commit) appends one receipt per commit,
    for the session whose shell ran it: the tree it landed in and a mark. Each
    consumer drains the receipts it has not seen, so two worktrees committed in
    one Bash call are both returned, and a refused commit, a commit by another
    session, and a commit in another tree each start nothing here.
    """
    if not isinstance(session_id, str) or not _SESSION_ID_RE.fullmatch(session_id) \
            or not re.fullmatch(r"[a-z-]{1,32}", consumer or ""):
        return []
    try:
        common = subprocess.check_output(
            ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
            text=True, timeout=5, stderr=subprocess.DEVNULL,
        ).strip()
        receipts = Path(common) / COMMIT_LANDED_DIR / session_id
        lines = receipts.read_text().splitlines()
    except Exception:
        return []
    seen_path = receipts.with_name(f"{session_id}.{consumer}")
    try:
        seen = int(seen_path.read_text())
    except (OSError, ValueError):
        seen = 0   # first use: nothing drained yet
    if not 0 <= seen <= len(lines):
        seen = 0
    try:
        seen_path.write_text(str(len(lines)))
    except OSError:
        return []   # a position that cannot be kept would act on every later call
    roots: list[str] = []
    for line in lines[seen:]:
        root, tab, mark = line.rpartition("\t")
        if tab and root and mark and root not in roots and os.path.isdir(root):
            roots.append(root)
    return roots


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
    root = _commit_target_root(cmd) or _repo_root()
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


# An uncovered source is a warning for the first STEP8_WARN_SECTIONS sections stamped
# after the rule shipped (commit STEP8_WARN_SINCE, 2026-11-12), then a refusal. A
# section counts when a commit adds its `> **Verified:**` stamp under todo/.
STEP8_WARN_SINCE = "d56710370b54e3e34c6ef8f3714cc2ae4ce84430"
STEP8_WARN_SECTIONS = 10


def _step8_sections_stamped(root: Path) -> int | None:
    """How many section stamps were added since STEP8_WARN_SINCE, or None when the
    history cannot say (a shallow clone without that commit)."""
    try:
        diff = subprocess.run(
            ["git", "log", "--format=", "-p", f"{STEP8_WARN_SINCE}..HEAD", "--", "todo/"],
            cwd=str(root), capture_output=True, text=True, timeout=3, check=True,
            errors="replace").stdout  # deadline: 3 s, a local history read
    except (OSError, subprocess.SubprocessError):
        return None
    return sum(1 for line in diff.splitlines() if line.startswith("+> **Verified:**"))


class _StagedReadError(Exception):
    """A staged blob that is in the index could not be read."""


# The hook has 10 s (.claude/settings.json) for every check; step 8's reads get this.
STEP8_READ_BUDGET_S = 4.0


def _staged_texts(root: Path, paths: list[str]) -> dict[str, str | None]:
    """The staged blob of each path, None for a path the index has no entry for (a
    deletion). One `ls-files` and one `cat-file --batch` under a shared deadline,
    however many paths. A failed or late read raises _StagedReadError: an unread file
    is missing evidence, never a file that needs no test.
    """
    deadline = time.monotonic() + STEP8_READ_BUDGET_S
    try:
        listing = subprocess.run(
            ["git", "--literal-pathspecs", "ls-files", "-s", "-z", "--", *paths],
            cwd=str(root), capture_output=True, timeout=STEP8_READ_BUDGET_S,
            check=True).stdout  # deadline: STEP8_READ_BUDGET_S, shared by both reads
        blob_of: dict[str, str] = {}
        for rec in listing.split(b"\0"):
            if rec:
                meta, path = rec.split(b"\t", 1)
                blob_of[path.decode("utf-8", "surrogateescape")] = meta.split()[1].decode()
        shas = sorted(set(blob_of.values()))
        content: dict[str, bytes] = {}
        if shas:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise subprocess.TimeoutExpired("git ls-files", STEP8_READ_BUDGET_S)
            data = subprocess.run(
                ["git", "cat-file", "--batch"], cwd=str(root), input="\n".join(shas).encode() + b"\n",
                capture_output=True, timeout=remaining,
                check=True).stdout  # deadline: what is left of STEP8_READ_BUDGET_S
            pos = 0
            for _ in shas:
                eol = data.index(b"\n", pos)
                sha, kind, size = data[pos:eol].split()
                if kind != b"blob":
                    raise ValueError(f"{sha.decode()} is a {kind.decode()}")
                start = eol + 1
                content[sha.decode()] = data[start:start + int(size)]
                pos = start + int(size) + 1
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        raise _StagedReadError(str(exc)) from exc
    return {p: (content[blob_of[p]].decode("utf-8", "replace") if p in blob_of else None)
            for p in paths}


def _step8_test_wiring_check(
    root: Path, staged_src: list[str], todo_files: list[str]
) -> tuple[bool, str]:
    """Step 8 (test wiring): every source the commit stages under src/{kernel,desktop,shell,
    apps}/ or user/ needs a test staged in the same commit that covers it: one that uses
    a public symbol the source defines, or that the source names in a `tested-by:` line
    (_test_coverage_by_symbol). A `**Note:** No <surface> test surface` line in a staged
    TODO exempts the commit. For the first STEP8_WARN_SECTIONS stamped sections an uncovered
    source only warns.
    SKIP_REVIEW_HOOK=1 and bootstrap mode are honored by the caller and below.
    """
    candidates = [
        p for p in staged_src
        if any(p.startswith(pre) for pre in _TEST_WIRING_SRC_PREFIXES)
        and not any(td in p for td in _TEST_WIRING_TEST_DIRS.values())
    ]
    if not candidates:
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
    import _test_coverage_by_symbol as tcs

    provenance = "src/libs/PROVENANCE.md"
    test_paths = [p for p in staged_src
                  if any(td in p for td in _TEST_WIRING_TEST_DIRS.values())
                  and re.search(r"test_[\w-]+\.c$", p)]
    wanted = [p for p in candidates if p.endswith(tcs.SOURCE_EXTS)]
    try:
        staged = _staged_texts(root, sorted({provenance, *tcs.RUNNERS, *wanted, *test_paths}))
    except _StagedReadError as exc:
        return (False, f"step-8 could not read the staged files, so their tests are unknown: {exc}")
    if any(p.startswith(tcs.VENDORED_ROOT) for p in wanted):
        local = tcs.local_additions(staged.get(provenance) or "")
        if local is None:
            return (False, f"step-8 cannot tell upstream ACPICA from our files: the staged "
                           f"{provenance} has no `## Local additions` heading")
    else:
        local = set()
    sources = {p: staged[p] for p in wanted
               if staged.get(p) is not None and not tcs.is_vendored(p, local)}
    if not sources:
        return (True, "")
    entry = tcs.runner_entry_points(staged.get(r) for r in tcs.RUNNERS)
    tests = {p: tcs.test_names(staged[p], entry) for p in test_paths if staged.get(p) is not None}
    uncovered = [p for p, text in sources.items() if not tcs.covering_tests(p, text, tests)]
    if not uncovered:
        return (True, "")
    for tf in todo_files:
        try:
            if _NO_TEST_SURFACE_RE.search((root / tf).read_text(encoding="utf-8")):
                return (True, "")
        except Exception:
            pass
    why = (
        f"{len(uncovered)} staged source(s) have no test in this commit that covers them: "
        f"{', '.join(uncovered[:5])}{' ...' if len(uncovered) > 5 else ''}. Stage a test "
        f"that calls what the source defines, add a `tested-by: test_<name>.c` line to a "
        f"source no test can call, or add a `**Note:** No <surface> test surface` line to "
        f"the TODO section. `python3 .claude/hooks/_test_coverage_by_symbol.py --report` "
        f"lists every kernel source's tests."
    )
    stamped = _step8_sections_stamped(root)
    if stamped is None or stamped < STEP8_WARN_SECTIONS:
        # fail-direction: history that cannot be read keeps the warning, since the
        # warning period is what an unknown count cannot prove has ended.
        seen = "an unknown number" if stamped is None else str(stamped)
        sys.stderr.write(f"[section-commit-gate] WARN (step-8, {seen} of "
                         f"{STEP8_WARN_SECTIONS} warning sections stamped; a refusal "
                         f"after that) -- {why}\n")
        return (True, "")
    return (False, why)


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

    # A section reviewed under the reviewer pin has no stamp: its step-13
    # evidence is a checked `round` receipt (_round_covers_ship). The stamp
    # file is therefore optional here; with neither, the refusal below names
    # both ways to supply the evidence.
    stamps_path = root / ".claude" / "state" / "last-review-stamps.json"
    state: dict = {}
    if stamps_path.exists():
        try:
            state = json.loads(stamps_path.read_text(encoding="utf-8"))
        except Exception as exc:
            return (False, f"last-review-stamps.json malformed ({exc})")
        if not isinstance(state, dict):
            return (False, "last-review-stamps.json is not a JSON object")

    now_ns = time.time_ns()
    # LEGACY FALLBACK ONLY -- see the module header: "these wall-clock TTLs apply
    # ONLY when the content binding is absent ... a slow review over unchanged
    # content never expires". This check was the one that never adopted that
    # rule; it aged out head-bound evidence unconditionally.
    #
    # MEASURED 2026-08-04 (v08, TODO-07 section 22): the step-13 stamp was
    # recorded at 07:32, correctly section-bound and HEAD-bound, and the commit
    # was refused at 08:05 -- because `implement-todo-section` step 16's OWN
    # mandatory chain (build 2m + test.sh 5m + test-tooling.sh 18m = ~25 min,
    # before TODO bookkeeping, the loose-end sweep and the todo-graph rebuild)
    # does not fit inside 30 minutes. The gate was refusing evidence for getting
    # old while the run did the work the skill told it to do, and the refusal
    # named the SECTION BINDING, so it read as "you never dispatched". That
    # misdirection cost a full re-dispatch (~6 min) carrying no new information,
    # and was filed a second time as a review-KIND mismatch -- which does not
    # reproduce: `adversarial` and `adversarial-impl` are both accepted here and
    # have been since 2026-06-30.
    #
    # Adopting `_dispatch_content_fresh` STRENGTHENS this gate rather than
    # relaxing it: age alone never invalidated the right thing, and a 5-minute
    # stamp over DRIFTED source used to pass and now does not.
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

    def _round_covers_ship(todo: str, required_section: str) -> bool:
        """A checked, complete `round` receipt for this section, dispatched on
        this history with nothing a pass reads COMMITTED since
        (_unread_by_final_pass: code, hooks, scripts and docs alike, not only
        kernel paths).

        Uncommitted edits made after the round are deliberately not compared.
        The round runs before the commit so that its findings can be fixed in
        the commit: requiring the committed tree to equal the tree it read
        would forbid applying its own findings without another round, and the
        wave after the commit already repeats until clean. What the STAMP
        requires is enforced where it is owned: _pass_covers binds the round,
        the performance pass and the last pass to the committed candidate.
        """
        receipt = _pass_receipts(root, todo, required_section)[0].get("round", (None, 0))[0]
        if receipt is None or not _receipt_complete(receipt) or not current_head:
            return False
        head = receipt["head"]
        if head == current_head:
            return True
        if not _git_is_ancestor(root, head, current_head):
            return False
        changed = _git_lines(root, "diff", "--no-renames", "--name-only", head, current_head)
        if changed is None:
            return False
        return not _unread_by_final_pass(root, changed, f"{head}:", f"{current_head}:")

    def _entry_has_matching_impl_adv(todo: str, required_section: str) -> bool:
        if _round_covers_ship(todo, required_section):
            return True
        entry = state.get(todo)
        if not isinstance(entry, dict):
            return False
        # Tag interchangeability (TODO-08 follow-up): `adversarial` is the
        # canonical implementation-time marker; `adversarial-impl` remains an
        # accepted alias/variant for older prompts. The head/section bindings
        # still apply per-tag.
        for tag in ("adversarial-impl", "adversarial"):
            ts = entry.get(tag)
            if not isinstance(ts, int):
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
            # CONTENT binding where it exists, wall-clock only where it does
            # not. `_dispatch_content_fresh` is the same rule the sibling
            # dispatch checks use: ancestry AND no source drift since, age
            # irrelevant. Without a recorded head there is nothing to bind to,
            # so the legacy TTL still applies.
            rh = entry.get(f"{tag}_head", "")
            if isinstance(rh, str) and rh and current_head:
                fresh, _why = _dispatch_content_fresh(root, rh, current_head)
                if not fresh:
                    continue
            elif ts < cutoff_ns:
                continue
            return True
        return False

    def _impl_adv_diagnosis(todo: str, required_section: str) -> str:
        """Why this section's step-13 evidence was rejected, in the gate's words."""
        entry = state.get(todo)
        if not isinstance(entry, dict):
            return "no dispatch entry recorded for this TODO"
        seen = []
        for tag in ("adversarial-impl", "adversarial"):
            ts = entry.get(tag)
            if not isinstance(ts, int):
                continue
            rec = _normalize_section_id(
                entry.get(f"{tag}_section") or entry.get("section", ""))
            if not rec:
                seen.append(f"`{tag}` recorded with NO section binding")
                continue
            if rec != required_section:
                seen.append(f"`{tag}` is bound to §{rec}, not §{required_section}")
                continue
            rh = entry.get(f"{tag}_head", "")
            if isinstance(rh, str) and rh and current_head:
                fresh, why = _dispatch_content_fresh(root, rh, current_head)
                if not fresh:
                    seen.append(f"`{tag}` §{rec} is content-STALE ({why})")
                    continue
            elif ts < cutoff_ns:
                age = int((now_ns - ts) / 60_000_000_000)
                seen.append(f"`{tag}` §{rec} has no recorded head and is "
                            f"{age} min old (legacy wall-clock TTL)")
                continue
        return "; ".join(seen) if seen else "no adversarial dispatch recorded"

    if required_sections:
        missing = [
            f"{todo} §{section}"
            for todo, section in required_sections
            if not _entry_has_matching_impl_adv(todo, section)
        ]
        if not missing:
            return (True, "")
        # Name WHY, not just WHAT. The old text said "missing <todo> §N" for
        # every rejection cause, so an expired-but-present stamp read as "you
        # never dispatched" -- which sent the run re-dispatching a review whose
        # findings were already closed, and produced a second, wrong filing that
        # blamed the review-KIND name. The diagnosis below distinguishes absent
        # from section-mismatched from drifted.
        return (False, (
            f"IO row [x] flip requires section-bound step-13 "
            f"`[review-kind: adversarial]` Codex evidence for each promoted "
            f"section; missing {', '.join(missing)}. "
            + "; ".join(
                f"{todo} §{section}: {_impl_adv_diagnosis(todo, section)}"
                for todo, section in required_sections
                if not _entry_has_matching_impl_adv(todo, section)
            )
            + f". Run: `bash scripts/codex-dispatch.sh "
              f"'[review-kind: adversarial] <todo-path> <section-N> <prompt>'`, "
              f"or for a section reviewed as passes of the reviewer pin, "
              f"`[review-kind: round] <todo-path> section <N>` "
              f"(.claude/skills/three-pass-review.md); a round counts once its "
              f"receipt is checked and complete."
        ))

    for todo in flipped_todos:
        entry = state.get(todo)
        if not isinstance(entry, dict):
            continue
        for tag in ("adversarial-impl", "adversarial"):
            ts = entry.get(tag)
            if not isinstance(ts, int):
                continue
            rh = entry.get(f"{tag}_head", "")
            if isinstance(rh, str) and rh and current_head:
                fresh, _why = _dispatch_content_fresh(root, rh, current_head)
                if not fresh:
                    continue
            elif ts < cutoff_ns:
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
        # Accept the Grep TOOL or an equivalent Bash grep/rg (v14 close-out,
        # 2026-08-16): the Grep tool is absent from some session schemas, so a
        # scan run through `Bash: grep/rg ...` used to leave this WARN
        # un-clearable no matter how the scan was done -- a warning that cannot
        # be satisfied honestly trains the run to ignore it. A Bash record's
        # `target` is the command line; require an actual grep/rg program plus
        # one of the markers so an unrelated Bash call does not clear it.
        marker = r"TODO|FIXME|HACK|STATUS_NOT_IMPLEMENTED"
        scan_grep = any(
            isinstance(r.get("ts_ns", 0), int)
            and r.get("ts_ns", 0) > latest_src_edit_ts
            and isinstance(r.get("target", ""), str)
            and re.search(marker, r.get("target", ""))
            and (r.get("tool_name") == "Grep"
                 or (r.get("tool_name") == "Bash"
                     and re.search(r"\b(?:grep|rg|egrep|fgrep)\b",
                                   r.get("target", ""))))
            for r in history
        )
        if not scan_grep:
            hm.emit_warn(
                str(root), 18,
                "loose-ends-scan-missing",
                "section commit pending but no scan for "
                "TODO|FIXME|HACK|STATUS_NOT_IMPLEMENTED recorded since "
                "the last src/ edit (Grep tool OR Bash grep/rg both count). "
                "implement-todo-section step 18: "
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
