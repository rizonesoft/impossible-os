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
_HOOK_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(_HOOK_DIR))
import codex_review_completed as crc  # noqa: E402

# ----------------------------------------------------------------------
# Constants
# ----------------------------------------------------------------------

EVIDENCE_TTL_SECONDS = 30 * 60  # 30 minutes for build + review
SKIP_REASON_MIN_LEN = 12

# Wrapper / env tokens we walk past to find the real `git` command.
# Same set as crc._CODEX_WRAPPER_TOKENS, with `git` stripped of any
# special-case wrappers it doesn't use.
_WRAPPER_TOKENS = crc._CODEX_WRAPPER_TOKENS | frozenset({"xargs"})

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
    # Walk past git's own pre-subcommand options.
    idx = 1
    git_value_flags = ("-c", "-C", "--git-dir", "--work-tree",
                        "--namespace", "--exec-path", "--super-prefix")
    while idx < len(out):
        tok = out[idx]
        if not tok.startswith("-"):
            break
        if tok in git_value_flags:
            if "=" in tok:
                idx += 1
            else:
                idx += 2
            continue
        if any(tok.startswith(f + "=") for f in git_value_flags):
            idx += 1
            continue
        idx += 1
    subcmd = out[idx] if idx < len(out) else ""
    # Codex H2 round-2: common commit aliases. Custom user-defined
    # aliases (`git -c alias.foo='commit ...' foo`) cannot be resolved
    # without parsing git config; documented as a known gap. The
    # ubiquitous `ci`/`cm` aliases ship with most developer setups.
    if subcmd not in ("commit", "ci", "cm"):
        return (False, False)
    # Codex H1: scan post-subcommand args for --no-verify / -n. The
    # `-n` short form is ambiguous (could be a value-arg of an
    # earlier flag), but in `git commit` argv positions after the
    # subcommand it is unambiguously --no-verify per `git-commit(1)`.
    has_no_verify = False
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


def _detect_signature(root: Path) -> tuple[str, list[str], list[str]]:
    """Returns (state, staged_source_files, flipped_todo_files) where
    state is one of:
      "section"     -- definitely a section commit (block on missing evidence)
      "not_section" -- definitely not a section commit (allow)
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
        return ("not_section", src, [])
    if not src or not todos:
        # One of the two halves is empty. Either way, no section
        # signature; skip the more-expensive flip detection.
        return ("not_section", src, [])
    flipped, todo_diff_ok = _impl_order_flips_with_status(root, todos)
    if flipped:
        return ("section", src, flipped)
    if not todo_diff_ok:
        # Some per-TODO diff failed to read. Cannot rule out a flip.
        return ("unknown", src, [])
    return ("not_section", src, [])


def _impl_order_flips_with_status(root: Path, todo_files: list[str]) -> tuple[list[str], bool]:
    """Wrap _impl_order_flips with a success flag. Returns
    (flipped, all_diffs_succeeded). Used by _detect_signature to
    distinguish "no flip detected because no diff had one" from
    "no flip detected because git couldn't return the diff".
    """
    flips: list[str] = []
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
        added_done: list[str] = []
        removed_open: list[str] = []
        flipped_here = False
        for line in diff.splitlines():
            if line.startswith("@@"):
                if added_done:
                    flipped_here = True
                    break
                added_done.clear()
                removed_open.clear()
                continue
            if line.startswith("---") or line.startswith("+++"):
                continue
            if _IO_OLD_OPEN_RE.match(line):
                removed_open.append(line)
            elif _IO_NEW_DONE_RE.match(line):
                added_done.append(line)
        if flipped_here or added_done:
            flips.append(path)
    return (flips, all_ok)


# ----------------------------------------------------------------------
# Evidence checks (post-signature; fail-CLOSED)
# ----------------------------------------------------------------------


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
    # 30-min wall-clock guardrail (mtime within window of now).
    age_s = time.time() - log_mtime
    if age_s > EVIDENCE_TTL_SECONDS:
        return (False, f"build/build.log mtime {age_s/60:.0f} min old "
                       f"(>{EVIDENCE_TTL_SECONDS/60:.0f} min TTL)")
    # Index/worktree desync check (Codex H2 round-2). `git diff
    # --name-only` (no --cached) reports paths whose worktree differs
    # from the index. If the build compiled the worktree but the index
    # has different content, the build is not evidence for the commit.
    desync = _index_worktree_desync(root, staged_src)
    if desync:
        return (False, f"staged content differs from worktree for "
                       f"{desync[:5]}; build compiled worktree, not index. "
                       f"Re-run build after staging or unstage divergent paths."
                       + (f" (+{len(desync)-5} more)" if len(desync) > 5 else ""))
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
        trig = str(state.get("trigger", "(unknown)"))[:80]
        return (False, f"latest Codex review (trigger: {trig}) was not "
                       f"processed through Skill(superpowers:receiving-code-review). "
                       f"Run the receive skill, then retry the commit.")
    rts = state.get("received_timestamp_ns")
    if not isinstance(rts, int):
        return (False, "received_timestamp_ns missing or not an int")
    age_s = (time.time_ns() - rts) / 1e9
    if age_s > EVIDENCE_TTL_SECONDS:
        return (False, f"Codex review received {age_s/60:.0f} min ago "
                       f"(>{EVIDENCE_TTL_SECONDS/60:.0f} min TTL); re-run review")
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
    uncovered = [p for p in staged_src if p not in trigger_blobs]
    if uncovered:
        return (False, f"review covered paths {sorted(trigger_blobs.keys())[:3]}... but "
                       f"commit includes uncovered source: {uncovered[:5]}"
                       + (f" (+{len(uncovered)-5} more)" if len(uncovered) > 5 else ""))
    mismatched = [
        p for p in staged_src
        if current_blobs.get(p) != trigger_blobs.get(p)
    ]
    if mismatched:
        return (False, f"review covered older content of {mismatched[:5]}; "
                       f"current staged blob SHA differs (post-review edit). "
                       f"Re-run the review against the new staging."
                       + (f" (+{len(mismatched)-5} more)" if len(mismatched) > 5 else ""))
    return (True, "")


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


def _is_skip_requested() -> tuple[bool, str | None, str | None]:
    """Returns (skip_requested, reason, error). If SKIP_REVIEW_HOOK=1
    is set BUT SKIP_REVIEW_HOOK_REASON is missing or too short, returns
    skip_requested=True with error explaining what's missing -- the
    caller blocks with a usage envelope. Both env vars together = real
    skip path.
    """
    if os.environ.get("SKIP_REVIEW_HOOK", "") != "1":
        return (False, None, None)
    reason = os.environ.get("SKIP_REVIEW_HOOK_REASON", "").strip()
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
    is_commit, has_no_verify, _cmd = _harness_command_is_git_commit()
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
    return _evaluate(root, mode="harness")


def _evaluate(root: Path, mode: str) -> int:
    """Shared signature + evidence evaluation. Pre-signature errors
    fail OPEN; post-signature errors and missing evidence fail CLOSED.
    """
    # SKIP path: handle BEFORE signature detection so a clearly opted-
    # out commit doesn't waste cycles on diff/regex work AND so an
    # incomplete opt-out (missing reason) is reported with a usage
    # envelope.
    skip_req, skip_reason, skip_err = _is_skip_requested()
    if skip_req and skip_err:
        sys.stderr.write(
            f"[section-commit-gate] BLOCK -- opt-out malformed: {skip_err}.\n"
            f"[section-commit-gate]   Required: SKIP_REVIEW_HOOK=1 AND "
            f"SKIP_REVIEW_HOOK_REASON=\"<text >= {SKIP_REASON_MIN_LEN} chars>\".\n"
        )
        return 2

    try:
        sig_state, staged_src, flipped = _detect_signature(root)
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
        return 0
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

    # Evidence checks (Codex H2 fail-closed scope).
    build_ok, build_err = _build_evidence(root, staged_src)
    review_ok, review_err = _review_evidence(root, staged_src)
    if build_ok and review_ok:
        return 0  # full evidence; commit allowed
    missing = []
    if not build_ok:
        missing.append(f"build evidence: {build_err}")
    if not review_ok:
        missing.append(f"Codex/receiving evidence: {review_err}")
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


def main(argv: list[str]) -> int:
    if "--git-hook-mode" in argv:
        root = _repo_root()
        if root is None:
            return 0  # not a git context; fail open
        return _git_hook_mode_main(root)
    return _harness_main()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
