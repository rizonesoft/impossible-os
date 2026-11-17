#!/usr/bin/env python3
# block-via: warning-only (HELPER module -- pure predicate + string
# extraction functions; no exit codes, no stderr writes).
"""Shared Codex dispatch detection + prompt extraction.

Owner: 00-infrastructure/TODO-08-automation-hardening (shell-aware Codex
dispatch segmentation section). Closes the substring-classifier
spoofing class identified in TODO-08 sections 25 and 28:

  1. Heredoc bodies. `cat <<EOF ... codex-companion.mjs adversarial-review
     "..." ... EOF` is a docs-write Bash command, not a Codex dispatch.
     The shlex tokenizer + per-segment heredoc-body strip in this module
     correctly classify it as not-a-dispatch.
  2. Multi-line dispatches. `node ... codex-companion.mjs \\<NL>
     adversarial-review "..."` is one logical command. The line-
     continuation pre-pass collapses `\\<NL>[ \\t]*` to a space before
     shlex tokenization so multi-line dispatches classify identically
     to their single-line equivalents.
  3. Wrapper-shape spoofing. `rg "codex-dispatch.sh '[review-kind: ...]'"`
     is a search command, not a dispatch. The shlex + argv shape match
     correctly classifies it as not-a-dispatch.

Three canonical Codex dispatch shapes are recognized:

  (a) Direct: `node ".../codex-companion.mjs" adversarial-review "<p>"`
  (b) Wrapper: `bash scripts/codex-dispatch.sh '<p>'` (TODO-08 section-28
      canonical shape)
  (c) Bare CLI: `codex review "<p>"` / `codex e "<p>"` (after walking
      past Codex global options)

All three preserve every consumer that previously consumed
`_is_codex_bash_trigger` / `_bash_prompt_arg` from
`codex_review_completed.py`.

Public API:
  is_codex_dispatch(cmd: str) -> bool
  extract_dispatch_prompt(cmd: str) -> str

Internal helpers (also exported for backward compat with the prior
`codex_review_completed.py` internal call sites):
  _trim_heredoc_body, _segment_by_separators, _segment_is_codex_invocation
"""

import re
import shlex


# Shell line-continuation pre-pass: bash collapses `\<NL>[ \t]*` to a
# single space at parse time, but a multi-line command typed verbatim
# in chat OR pulled from a docs example arrives at PostToolUse with
# the raw `\<newline>` still present. Normalize before tokenizing so
# the segmenter sees the logically-joined command.
_LINE_CONT_RE = re.compile(r"\\\r?\n[ \t]*")


# Heredoc-into-variable pre-pass. The canonical apostrophe-safe Codex
# dispatch shape from `codex-prompt-template.md` is:
#
#     PROMPT=$(cat <<'EOF'
#     [review-kind: gap-audit] todo/...
#     <body, free to use apostrophes/backticks/parens>
#     EOF
#     )
#     bash scripts/codex-dispatch.sh "$PROMPT"
#
# After shlex splits this, the dispatch invocation's prompt argument is
# the literal string `$PROMPT` (shlex preserves the dollar-name without
# expansion). Without this pre-pass the kind extractor returns "" because
# the first non-blank line of `$PROMPT` carries no `[review-kind:]`.
#
# The regex walks BEFORE tokenization to capture the heredoc body as a
# single string; the harvested map is consulted in `_segment_extract_prompt`
# whenever the extracted argv looks like `$VAR` or `${VAR}`.
#
# Multiple assignments in the same command keep their bodies separate
# (the dict overwrites with the most recent value, matching shell
# semantics). Unmatched / unterminated heredocs are silently ignored
# so a malformed dispatch still flows through the legacy path without
# raising.
_HEREDOC_VAR_RE = re.compile(
    r"(?ms)"
    r"^[ \t]*([A-Za-z_]\w*)=\$\(\s*cat\s*<<-?\s*"   # VAR=$(cat <<
    r"['\"]?([A-Za-z_]\w*)['\"]?"                    # delimiter (with optional quotes)
    r"\s*\n"                                         # newline before body
    r"(.*?)"                                         # body (non-greedy)
    r"^\2[ \t]*\n"                                   # delimiter alone on its line
    r"[ \t]*\)"                                      # closing paren
)


def _harvest_heredoc_vars(cmd):
    """Return `(stripped_cmd, {varname: heredoc_body})` for every
    `VAR=$(cat <<'X' ... X)` block in `cmd`. The block is REMOVED
    from the returned command so shlex tokenization isn't poisoned
    by the `<<` token (which `_trim_heredoc_body` would otherwise
    treat as the start of an unterminated heredoc, killing every
    later Codex invocation token).

    Body strings have trailing newlines preserved -- the kind
    extractor only inspects the first non-blank line.
    """
    if not isinstance(cmd, str) or "<<" not in cmd or "=$(" not in cmd:
        return (cmd, {})
    out = {}

    def _replace(m):
        out[m.group(1)] = m.group(3)
        return ""  # strip the whole block; keep newlines around it

    stripped = _HEREDOC_VAR_RE.sub(_replace, cmd)
    return (stripped, out)


def _resolve_var(prompt, heredoc_vars):
    """If `prompt` is exactly `$VAR` or `${VAR}` and `VAR` appears in
    `heredoc_vars`, return the heredoc body. Otherwise return `prompt`
    unchanged. Tolerates surrounding whitespace because shlex preserves
    `"$PROMPT"` as the literal `$PROMPT` (quotes stripped, no expansion).
    """
    if not prompt or not heredoc_vars:
        return prompt
    s = prompt.strip()
    if s.startswith("${") and s.endswith("}"):
        name = s[2:-1]
    elif s.startswith("$"):
        name = s[1:]
    else:
        return prompt
    if name in heredoc_vars:
        return heredoc_vars[name]
    return prompt


# Bash control operators that introduce a NEW command sequence.
# `true && codex review prompt` was a bypass: the original code
# treated the whole command as one argv with `true` at position 0.
# Mirrors the codex_model_flag_block.py pattern.
_COMMAND_SEPARATORS = ("&&", "||", ";", "|", "&")

# Tokens that are env-prefix assignments (FOO=bar) or wrapper commands
# we walk past when looking for the "real" Codex invocation. Includes
# taskset / chrt / setsid / setpriv / cgexec / flock for parity with
# section_commit_gate.py + codex_model_flag_block.py wrapper-token sets.
_CODEX_WRAPPER_TOKENS = frozenset({
    "sudo", "doas", "env", "nice", "nohup", "timeout", "ionice",
    "stdbuf", "unbuffer", "chronic", "exec", "command",
    "taskset", "chrt", "setsid", "setpriv", "cgexec", "flock",
})


def _trim_heredoc_body(tokens):
    """Return tokens up to (but not including) the first construct
    that introduces SHELL SUB-CONTENT (heredoc body, process
    substitution, command substitution body). Tokens after the
    opener are NOT part of the real command line.
    """
    for i, tok in enumerate(tokens):
        if tok in ("<<", "<<-") or tok.startswith("<<"):
            return tokens[:i]
        if tok.startswith("<(") or tok.startswith(">("):
            return tokens[:i]
        if tok.startswith("$(") or tok.startswith("${"):
            return tokens[:i]
        if tok.startswith("`"):
            return tokens[:i]
    return tokens


def _segment_by_separators(tokens):
    """Split tokens by Bash control operators into a list of simple
    commands. `true && codex review prompt` -> [['true'], ['codex',
    'review', 'prompt']]. Each segment is its own argv to scan.
    """
    segs = []
    cur = []
    for tok in tokens:
        if tok in _COMMAND_SEPARATORS:
            if cur:
                segs.append(cur)
            cur = []
        else:
            cur.append(tok)
    if cur:
        segs.append(cur)
    return segs


# Per-wrapper value-flag table. Used to walk past `--flag value`
# cleanly; `--flag=value` attached form is handled by detecting
# `=` in the flag token. Sourced from codex_review_completed.py
# at the time of the section-30 helper extraction.
_WRAPPER_VALUE_FLAGS = {
    "sudo": frozenset({"-u", "-g", "-p", "-r", "-h", "-D", "-C"}),
    "doas": frozenset({"-u", "-C"}),
    "timeout": frozenset({"-k", "--kill-after", "-s", "--signal"}),
    "nice": frozenset({"-n", "--adjustment"}),
    "ionice": frozenset({"-c", "--class", "-n", "--classdata", "-p", "--pid", "-P", "-u"}),
    "env": frozenset({"-u", "--unset", "-S", "--split-string", "-C", "--chdir"}),
    "stdbuf": frozenset({"-i", "-o", "-e"}),
    "command": frozenset(),
    "taskset": frozenset({"-c", "--cpu-list", "-p", "--pid"}),
    "chrt": frozenset({"-p", "--pid"}),
    "setpriv": frozenset({"--reuid", "--regid",
                          "--ruid", "--euid", "--rgid", "--egid",
                          "--groups", "--inh-caps", "--ambient-caps",
                          "--bounding-set", "--securebits",
                          "--pdeathsig", "--selinux-label",
                          "--apparmor-profile"}),
    "cgexec": frozenset({"-g"}),
    "flock": frozenset({"-w", "--timeout", "-E", "--conflict-exit-code", "-c", "--command"}),
}

_COMPANION_REVIEW_SUBCOMMANDS = frozenset({
    "review", "adversarial-review", "task",
})

# Companion `task` (incl. --background) and the bg wrapper START a job; the
# receive step can fire before the job finishes, so their receipt is dispatch
# TELEMETRY, never completed-review proof (the ledger mirror must not record
# result=received for them).


def _segment_is_background(seg_tokens):
    """True when THIS dispatch segment's invocation is companion `task` or the
    bg wrapper. Argv-based on purpose: a raw-text scan misclassified foreground
    dispatches whose PROMPT merely mentions the bg wrapper (e.g. a review of
    the background-dispatch code itself)."""
    out = _strip_env_and_wrappers(list(seg_tokens))
    if not out:
        return False
    head = out[0]
    second = out[1] if len(out) > 1 else ""
    third = out[2] if len(out) > 2 else ""
    head_base = head.rsplit("/", 1)[-1]
    if "codex-companion.mjs" in head_base:
        return second == "task"
    if head_base == "node" and "codex-companion.mjs" in second:
        return third == "task"
    if head_base in ("bash", "sh", "/bin/bash", "/bin/sh") and \
            second.endswith("codex-bg-dispatch.sh"):
        return True
    if head_base == "codex-bg-dispatch.sh" or head_base.endswith("/codex-bg-dispatch.sh"):
        return True
    # The review broker DETACHES its review (systemd-run/setsid) and returns a
    # {logFile} immediately -- so it is a background dispatch, and its instant
    # return must not read as a completed review (P1.3). NOTE: the broker is a
    # real REVIEW wrapper, so the recorder keeps stamping it via the
    # is_review_broker_dispatch() exception -- flagging it background here only
    # makes last-codex-review.json's background_dispatch flag honest.
    if head_base in ("bash", "sh", "/bin/bash", "/bin/sh") and \
            second.endswith("review-broker-codex-dispatch.sh"):
        return True
    if head_base.endswith("review-broker-codex-dispatch.sh"):
        return True
    return False


def is_review_broker_dispatch(cmd: str) -> bool:
    """True when `cmd` invokes review-broker-codex-dispatch.sh. The broker is a
    background REVIEW dispatch (see _segment_is_background); the recorder uses
    this to KEEP stamping it even though it is flagged background, since a
    completed broker leg is genuine review proof (a generic `task --background`
    is not)."""
    if not isinstance(cmd, str) or "review-broker-codex-dispatch.sh" not in cmd:
        return False
    toks = _tokenize(_harvest_heredoc_vars(cmd)[0])
    for seg in _segment_by_separators(toks):
        seg = _trim_heredoc_body(_strip_env_and_wrappers(list(seg)))
        for tok in seg[:2]:
            if tok.endswith("review-broker-codex-dispatch.sh"):
                return True
    return False


def is_background_dispatch(cmd: str) -> bool:
    """True when the FIRST matching Codex dispatch segment in `cmd` starts a
    background job rather than completing a review in the foreground."""
    if not isinstance(cmd, str) or not cmd.strip() or "codex" not in cmd:
        return False
    stripped, _heredoc_vars = _harvest_heredoc_vars(cmd)
    toks = _tokenize(stripped)
    if not toks:
        return False
    for seg in _segment_by_separators(toks):
        seg = _trim_heredoc_body(seg)
        if seg and _segment_is_codex_invocation(seg):
            return _segment_is_background(seg)
    return False

# Review-carrying bare CLI subcommands ONLY. `codex e` / `codex exec` are
# non-review automation: trusting the `e` alias let a mutating session mint
# reviewer evidence by
# running `codex e '[review-kind: ...] ...'` through the receipt path.
_CODEX_BARE_SUBCMDS = ("review",)


def _is_duration(tok):
    if not tok:
        return False
    if tok[0].isdigit():
        if tok[-1] in "smhd" and len(tok) > 1:
            return tok[:-1].replace(".", "", 1).isdigit()
        return tok.replace(".", "", 1).isdigit()
    return False


def _is_assignment(tok):
    if "=" not in tok or tok.startswith("="):
        return False
    head = tok.split("=", 1)[0]
    return bool(head) and (head[0].isalpha() or head[0] == "_") and all(
        c.isalnum() or c == "_" for c in head)


def _skip_env_and_wrappers(out, i=0):
    """Index of the first token of `out[i:]` past env-prefix assignments and
    wrapper-command prefixes (sudo, env, timeout, ...). Reads, never copies."""
    n = len(out)
    while i < n and _is_assignment(out[i]):
        i += 1
    while i < n and out[i] in _CODEX_WRAPPER_TOKENS:
        wrapper = out[i]
        i += 1
        value_flags = _WRAPPER_VALUE_FLAGS.get(wrapper, frozenset())
        while i < n and out[i].startswith("-"):
            flag = out[i]
            i += 1
            if "=" in flag:
                continue
            if flag in value_flags and i < n:
                i += 1
        # Wrapper-specific positional args before the real command.
        if wrapper == "timeout" and i < n and _is_duration(out[i]):
            i += 1
        elif wrapper == "env":
            while i < n and _is_assignment(out[i]):
                i += 1
        elif wrapper == "nice" and i < n and out[i].lstrip("-").isdigit():
            i += 1
        elif wrapper == "chrt" and i < n and out[i].isdigit():
            i += 1
    return i


def _strip_env_and_wrappers(out):
    """Mutate `out` (a list of tokens) by stripping leading env-prefix
    assignments + wrapper-command prefixes (sudo, env, timeout, ...).
    Returns the mutated list. Same shape as the prior
    codex_review_completed.py inline implementation.
    """
    del out[:_skip_env_and_wrappers(out)]
    return out


# Words bash reads as grammar in front of a simple command, so the command a
# safety guard must judge comes after them (2026-11-17: `if true; then git reset
# --hard; fi` was read as the command `then` and allowed).
_GRAMMAR_HEADS = frozenset({"if", "then", "else", "elif", "do", "while", "until", "!", "{", "time"})
# A redirection word (`>/dev/null`, `2>&1`, `<in`, `&>log`); with nothing after
# the operator, the next word is its target. Bash removes both from the argv,
# wherever they stand (`>/dev/null git reset --hard`, `git 2>/dev/null reset`).
# Longer operators first, so `>&` is not read as `>` with target `&`.
_REDIRECT_RE = re.compile(r"\A(?:\d+|&)?(?:>>|>&|>\||<<<|<>|<&|>|<)(.*)\Z", re.S)
_REDIRECT_CHARS = frozenset("<>")


def _unquoted_at(tok, j):
    """True when character j of word `tok` stood outside quotes in the command.
    A word without a mask (one this module built itself) counts as unquoted."""
    mask = getattr(tok, "unquoted", None)
    return mask is None or mask[j] == 1


def _split_attached_redirects(out):
    """Split each word at its first UNQUOTED `<` or `>` (with an unquoted `&`
    before it): `git>/dev/null` is the word `git` and the redirection
    `>/dev/null`, while `bash -c 'echo ok >/dev/null; ...'` stays one word and
    `2>"reset errors.txt"` is one redirection. Returns the words and, beside
    them, whether each one starts a redirection."""
    words, redir = [], []
    for tok in out:
        start = next((j for j, c in enumerate(tok) if c in _REDIRECT_CHARS and _unquoted_at(tok, j)), None)
        if start is None:
            words.append(tok)
            redir.append(False)
            continue
        if start > 0 and tok[start - 1] == "&" and _unquoted_at(tok, start - 1):
            start -= 1
        head = tok[:start]
        if head and not head.isdigit():
            words.append(head)
            redir.append(False)
        words.append(tok if not head or head.isdigit() else tok[start:])
        redir.append(True)
    return words, redir


def _redirect_width(out, redir, i):
    """How many words starting at out[i] are one redirection (0 when none)."""
    if not redir[i]:
        return 0
    m = _REDIRECT_RE.match(out[i])
    if not m:
        return 0
    return 1 if m.group(1) or i + 1 >= len(out) else 2


def guard_argv(seg):
    """The argv bash runs for segment `seg`, for the Bash safety guards.

    Skips what `_strip_env_and_wrappers` strips, plus reserved words, `time -p`,
    a `case WORD in PATTERN)` lead, a later arm's `PATTERN)` (also written
    compactly, `x)git`), and subshell parens: a leading `(` and a trailing `)` on
    any argument, so `(git reset --hard) 2>/dev/null` still reads `--hard`.
    Returns [] when the segment holds no command. One forward pass over the
    tokens: each is examined a bounded number of times. The Codex dispatch
    recogniser does not use this: its prompt arguments must stay byte-exact.
    """
    out, redir = _split_attached_redirects(seg)
    n = len(out)
    i = _skip_env_and_wrappers(out, 0)
    while i < n:
        tok = out[i]
        width = _redirect_width(out, redir, i)
        if width:
            i += width
        elif tok in _GRAMMAR_HEADS:
            i += 1
            if tok == "time":
                while i < n and out[i] in ("-p", "--"):
                    i += 1
        elif tok == "case":
            while i < n and out[i] != "in":
                i += 1
            i += 1
            while i < n and ")" not in out[i]:
                i += 1
            if i >= n:
                return []
            # the token holding `)` is cut below, then prefixes are skipped
            out[i] = out[i].rpartition(")")[2]
            if not out[i]:
                i += 1
        elif tok.startswith("("):
            # a subshell, or `(PATTERN)` / `(PATTERN)cmd` in a case arm
            body = tok.lstrip("(")
            out[i] = body.rpartition(")")[2] if ")" in body else body
            if not out[i]:
                i += 1
        elif ")" in tok:
            # a later case arm's PATTERN) or PATTERN)cmd: an unquoted `)` ends a
            # pattern and is never part of a command name; cut at the last one
            out[i] = tok.rpartition(")")[2]
            if not out[i]:
                i += 1
        else:
            break
        # every rewrite above can expose `env`, `sudo` or `FOO=1` as the next head
        i = _skip_env_and_wrappers(out, i)
    argv = []
    while i < n:
        width = _redirect_width(out, redir, i)
        if width:
            i += width
            continue
        tok = out[i]
        i += 1
        if tok.endswith(")"):
            tok = tok.rstrip(")")
            if not tok:
                continue
        argv.append(tok)
    return argv


def _walk_codex_global_options(out, start=1):
    """For a `codex` head token, walk past Codex CLI global options
    (-c, --config, --enable, etc.) returning the index of the real
    subcommand. Mirrors codex_model_flag_block.py
    CODEX_GLOBAL_VALUE_FLAGS.
    """
    codex_global_value_flags = (
        "-c", "--config", "--enable", "--disable", "--remote",
        "--bearer-token-env-var",
    )
    idx = start
    while idx < len(out):
        tok = out[idx]
        if not tok.startswith("-"):
            break
        if tok in codex_global_value_flags:
            if "=" in tok:
                idx += 1
            else:
                idx += 2
            continue
        idx += 1
    return idx


def _segment_is_codex_invocation(seg_tokens):
    """Test whether a SINGLE command segment (already separated from
    operator chains and stripped of heredoc body) is a Codex dispatch.

    Three canonical shapes recognized:
      (a) `<path>/codex-companion.mjs <subcmd> ...`
      (b) `node <path>/codex-companion.mjs <subcmd> ...`
      (c) `bash|sh <path>/codex-dispatch.sh ...` OR bare codex-dispatch.sh
      (d) `codex [global-flags] <subcmd> ...`

    Returns True iff the head argv (after env-prefix + wrapper-prefix
    strip) matches one of these shapes AND the subcommand is a real
    review subcommand (not metadata/control).
    """
    out = _strip_env_and_wrappers(list(seg_tokens))
    if not out:
        return False
    head = out[0]
    second = out[1] if len(out) > 1 else ""
    third = out[2] if len(out) > 2 else ""
    head_base = head.rsplit("/", 1)[-1]

    if "codex-companion.mjs" in head_base:
        return second in _COMPANION_REVIEW_SUBCOMMANDS
    if head_base == "node" and "codex-companion.mjs" in second:
        return third in _COMPANION_REVIEW_SUBCOMMANDS
    if head_base in ("bash", "sh", "/bin/bash", "/bin/sh") and \
            (second.endswith("codex-dispatch.sh") or second.endswith("codex-bg-dispatch.sh")
             or second.endswith("review-broker-codex-dispatch.sh")):
        return True
    if head_base in ("codex-dispatch.sh", "codex-bg-dispatch.sh",
                     "review-broker-codex-dispatch.sh") or \
            head_base.endswith("/codex-dispatch.sh") or head_base.endswith("/codex-bg-dispatch.sh") \
            or head_base.endswith("/review-broker-codex-dispatch.sh"):
        return True
    if head_base == "codex":
        idx = _walk_codex_global_options(out, start=1)
        subcmd = out[idx] if idx < len(out) else ""
        if subcmd in _CODEX_BARE_SUBCMDS:
            return True
    return False


def _segment_extract_prompt(seg_tokens):
    """Return the prompt argv from a segment that
    _segment_is_codex_invocation returned True on. Returns empty
    string if the prompt cannot be located.

    The prompt is the FIRST positional after the review subcommand.
    Walks past per-subcommand flags such as `task --background --json`.
    """
    out = _strip_env_and_wrappers(list(seg_tokens))
    if not out:
        return ""

    def _first_positional_after(seg, start):
        idx = start
        while idx < len(seg) and seg[idx].startswith("-"):
            idx += 1
        return seg[idx] if idx < len(seg) and not seg[idx].startswith("-") else ""

    for i, tok in enumerate(out):
        base = tok.rsplit("/", 1)[-1]
        if "codex-companion.mjs" in base:
            if i + 1 < len(out) and out[i + 1] in _COMPANION_REVIEW_SUBCOMMANDS:
                return _first_positional_after(out, i + 2)
            return ""
        if base == "node" and i + 1 < len(out) and "codex-companion.mjs" in out[i + 1]:
            if i + 2 < len(out) and out[i + 2] in _COMPANION_REVIEW_SUBCOMMANDS:
                return _first_positional_after(out, i + 3)
            return ""
        if base in ("bash", "sh", "/bin/bash", "/bin/sh") and i + 1 < len(out) and \
                (out[i + 1].endswith("codex-dispatch.sh") or out[i + 1].endswith("codex-bg-dispatch.sh")
                 or out[i + 1].endswith("review-broker-codex-dispatch.sh")):
            return _first_positional_after(out, i + 2)
        if base in ("codex-dispatch.sh", "codex-bg-dispatch.sh",
                    "review-broker-codex-dispatch.sh") or \
                base.endswith("/codex-dispatch.sh") or base.endswith("/codex-bg-dispatch.sh") \
                or base.endswith("/review-broker-codex-dispatch.sh"):
            return _first_positional_after(out, i + 1)
        if base == "codex":
            idx = _walk_codex_global_options(out, start=i + 1)
            if idx < len(out) and out[idx] in _CODEX_BARE_SUBCMDS:
                return _first_positional_after(out, idx + 1)
            return ""
    return ""


def _unquoted_newlines_to_sep(cmd):
    """Convert UNQUOTED newlines to ` ; ` so shlex/`_segment_by_separators`
    treat them as command separators (bash terminates a command on a bare
    newline). Newlines inside single/double quotes are preserved (a multi-line
    prompt arg stays one token). Heredoc bodies are already stripped by
    `_harvest_heredoc_vars` before `_tokenize` runs. Without this a
    `cd <dir>\\nbash scripts/codex-dispatch.sh '...'` compound collapsed into
    one `cd`-led segment and the real dispatch went undetected/unrecorded.
    """
    out = []
    quote = None
    i = 0
    n = len(cmd)
    while i < n:
        c = cmd[i]
        if quote:
            out.append(c)
            if quote == '"' and c == "\\" and i + 1 < n:
                out.append(cmd[i + 1])
                i += 2
                continue
            if c == quote:
                quote = None
        elif c in ("'", '"'):
            quote = c
            out.append(c)
        elif cmd.startswith(("<<", "$(", "<(", ">("), i) or c == "`":
            # A multi-line body opener (heredoc / command-substitution /
            # process-substitution): its inner newlines are NOT command
            # separators and `_trim_heredoc_body` owns them at the segment
            # level. Stop converting and pass the remainder through untouched.
            out.append(cmd[i:])
            break
        elif c == "\n":
            out.append(" ; ")
        elif c in (";", "|", "&"):
            # Space-pad UNQUOTED control operators so shlex emits them as their
            # own tokens.
            #
            # shlex.split only breaks on whitespace, so an operator written
            # WITHOUT surrounding spaces stays glued to its neighbour:
            # `cd /tmp; codex ...` tokenizes as [..., '/tmp;', 'codex', ...] and
            # `true&&codex ...` as ['true&&codex', ...]. `_segment_by_separators`
            # already lists ';' '|' '&' '&&' '||' -- it simply never sees one, so
            # the whole compound reads as a single `cd`/`true`-led segment and
            # the dispatch inside it is invisible.
            #
            # Direction differs by consumer and BOTH are wrong. For the review
            # gates it fails closed (a performed review goes unrecognized, so
            # correct work is BLOCKED and the operator is trained to reach for
            # SKIP_*_HOOK). For codex_model_flag_block it fails OPEN: measured
            # 2026-07-27, `true&&codex exec --model gpt-4` and
            # `cd /tmp;codex exec --model gpt-4` were both ALLOWED while their
            # spaced equivalents were BLOCKED -- a policy bypass, which is the
            # exact substring-classifier weakness this module exists to close.
            #
            # Padding happens HERE, inside the existing quote-aware walk, rather
            # than by switching to shlex(punctuation_chars=True): that would also
            # re-tokenize redirects (`2>&1` -> '2','>&','1') and change shapes
            # every downstream consumer already agrees on. This keeps quoting
            # semantics exactly as they were -- an operator inside a quoted
            # prompt is never touched, because this branch is unreachable while
            # `quote` is set.
            prev = cmd[i - 1] if i > 0 else ""
            nxt = cmd[i + 1] if i + 1 < n else ""
            if c == "&" and (prev in (">", "<") or nxt == ">"):
                # Part of a redirect (`2>&1`, `&>log`), not a control operator.
                out.append(c)
            elif c in ("&", "|") and nxt == c:
                out.append(" " + c + c + " ")   # && / ||
                i += 2
                continue
            else:
                out.append(" " + c + " ")
        else:
            out.append(c)
        i += 1
    return "".join(out)


def _tokenize(cmd):
    """Pre-process line-continuations + shlex tokenize the command.
    Returns empty list on shlex error.
    """
    if not isinstance(cmd, str) or not cmd.strip():
        return []
    cmd = _LINE_CONT_RE.sub(" ", cmd)
    cmd = _unquoted_newlines_to_sep(cmd)
    try:
        toks = shlex.split(cmd, posix=True, comments=False)
    except ValueError:
        return []
    return _with_quote_masks(cmd, toks)


class _Word(str):
    """A token that remembers which of its characters stood outside quotes
    (`unquoted[j] == 1`), so the safety guards can tell the redirection in
    `git 2>"reset errors.txt" reset` from the text of a quoted `bash -c` body.
    It compares and hashes as the plain str it is; a slice is a plain str.
    """


def _with_quote_masks(cmd, toks):
    """`toks` (shlex's split of `cmd`) as _Word tokens carrying quote masks.
    One linear pass over `cmd` with the POSIX rules shlex applies; if its words
    differ from shlex's in any way, `toks` is returned unmasked, which the
    guards read as all unquoted (the stricter reading)."""
    words, cur, mask, started = [], [], bytearray(), False
    i, n, quote = 0, len(cmd), None
    while i < n:
        c = cmd[i]
        if quote == "'":
            if c == "'":
                quote = None
            else:
                cur.append(c)
                mask.append(0)
        elif quote == '"':
            if c == '"':
                quote = None
            elif c == "\\" and i + 1 < n and cmd[i + 1] in '\\"':  # shlex escapes only these
                i += 1
                cur.append(cmd[i])
                mask.append(0)
            else:
                cur.append(c)
                mask.append(0)
        elif c in " \t\r\n":
            if started:
                words.append((cur, mask))
                cur, mask, started = [], bytearray(), False
        elif c in "'\"":
            quote, started = c, True
        elif c == "\\" and i + 1 < n:
            i += 1
            cur.append(cmd[i])
            mask.append(0)
            started = True
        else:
            cur.append(c)
            mask.append(1)
            started = True
        i += 1
    if started:
        words.append((cur, mask))
    if len(words) != len(toks):
        return toks
    out = []
    for (chars, m), tok in zip(words, toks):
        text = "".join(chars)
        if text != tok:
            return toks
        w = _Word(text)
        w.unquoted = bytes(m)
        out.append(w)
    return out


def is_codex_dispatch(cmd):
    """True iff `cmd` contains a real Codex dispatch in some segment.

    Shell-aware: shlex tokenize + segment-by-control-operator + per-
    segment heredoc-body strip + argv shape match. False positives
    from substring matches in heredoc bodies, prose, and search
    commands are eliminated.

    Performance: short-circuits via `if "codex" not in cmd: return
    False` before any expensive parsing. >99% of Bash calls don't
    mention codex; the substring screen is bounded under 1us.
    """
    if not isinstance(cmd, str) or not cmd.strip():
        return False
    if "codex" not in cmd:
        return False
    stripped, _ = _harvest_heredoc_vars(cmd)
    toks = _tokenize(stripped)
    if not toks:
        return False
    for seg in _segment_by_separators(toks):
        seg = _trim_heredoc_body(seg)
        if seg and _segment_is_codex_invocation(seg):
            return True
    return False


def extract_dispatch_prompt(cmd):
    """Return the prompt argv from the first matching Codex dispatch
    segment in `cmd`, or empty string. Uses the same shell-aware
    parsing as `is_codex_dispatch`.

    Resolves `VAR=$(cat <<'X' BODY X)` + `... "$VAR"` indirection
    (the canonical apostrophe-safe shape from
    `codex-prompt-template.md`) so the kind extractor can read
    `[review-kind: ...]` from the heredoc body instead of seeing the
    literal `$VAR` token.
    """
    prompts = extract_dispatch_prompts(cmd)
    return prompts[0] if prompts else ""


def extract_dispatch_prompts(cmd):
    """EVERY Codex dispatch prompt in `cmd`, in order. Same parsing as the
    singular form, which is now the first element of this list.

    A compound Bash call can carry several dispatches, and the singular
    extractor stopped at the first -- so the rest were invisible to every
    consumer. MEASURED 2026-08-03: `review-todo-section` step 8's two quality
    dispatches (consistency + perf) were issued in ONE Bash call alongside the
    step-5 adversarial one -- all three broker artifacts carry the same
    `20260803-101826` second-resolution stamp -- and only `adversarial` was
    attributed. The commit was then refused with "step(s) [8] were never
    observed" against recorded steps [4, 5, 17], and the run took a
    `SKIP_SKILL_STEP_BLOCK` opt-out on a review it had FULLY performed: 15,714
    bytes of consistency findings and 10,912 of perf, five findings triaged.
    That is the worst outcome a gate can produce -- it trains the run to skip a
    real check to get past a bookkeeping miss.

    NOTE the filed diagnosis blamed step 8's matcher for recognising only the
    direct `scripts/codex-dispatch.sh` shape and not the broker. It does not
    reproduce: the broker resolves identically to the direct shape for all four
    kinds (the basename-suffix contract in `_review_kind`), and step 5 was
    attributed from that same broker in the same call. First-match truncation
    is the mechanism; the shape was never the problem.
    """
    if not isinstance(cmd, str) or not cmd.strip():
        return []
    if "codex" not in cmd:
        return []
    stripped, heredoc_vars = _harvest_heredoc_vars(cmd)
    toks = _tokenize(stripped)
    if not toks:
        return []
    out = []
    for seg in _segment_by_separators(toks):
        seg = _trim_heredoc_body(seg)
        if seg and _segment_is_codex_invocation(seg):
            prompt = _resolve_var(_segment_extract_prompt(seg), heredoc_vars)
            if prompt:
                out.append(prompt)
    return out
