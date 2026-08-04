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


def _strip_env_and_wrappers(out):
    """Mutate `out` (a list of tokens) by stripping leading env-prefix
    assignments + wrapper-command prefixes (sudo, env, timeout, ...).
    Returns the mutated list. Same shape as the prior
    codex_review_completed.py inline implementation.
    """
    # Strip leading env-assignments.
    while out and "=" in out[0] and not out[0].startswith("="):
        head = out[0].split("=", 1)[0]
        if head and (head[0].isalpha() or head[0] == "_") and all(
            c.isalnum() or c == "_" for c in head
        ):
            out.pop(0)
        else:
            break
    # Walk wrappers.
    while out and out[0] in _CODEX_WRAPPER_TOKENS:
        wrapper = out.pop(0)
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
        elif wrapper == "env":
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
    return out


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
        return shlex.split(cmd, posix=True, comments=False)
    except ValueError:
        return []


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


def command_segments(cmd):
    """Split `cmd` into shell segments of RAW SOURCE TEXT, each classified as a
    Codex dispatch. Returns `[(raw_text, is_codex), ...]`, or `[]` when the
    command cannot be split (a caller then falls back to whole-command
    handling).

    This exists because `is_codex_dispatch` answers "does this command CONTAIN
    a dispatch", which is the right question for attributing a review receipt
    and the WRONG one for granting an exemption: a gate that exempts the whole
    command line on that answer lets `codex-dispatch.sh '<prompt>' && <the
    thing being gated>` through on the strength of its first segment. A gate
    wants per-segment truth -- exempt the dispatch, still judge everything
    chained after it.

    The segments are RAW SLICES of the original string, never a `shlex.join`
    round-trip. Re-quoting is lossy in exactly the way a caller's regexes care
    about: `(bash scripts/test.sh)` tokenizes to `(`, `bash`, ... and rejoins
    as `'(bash' scripts/test.sh`, which puts a quote between `bash` and its
    argument and moves the script off a command position. A caller matching
    invocation shapes then sees nothing, and a real bare suite run inside a
    subshell or a `$(...)` substitution sails through. Both shapes were
    verified to escape before this was changed back to raw slices.
    """
    if not isinstance(cmd, str) or not cmd.strip():
        return []
    if _has_unmodelled_grammar(cmd):
        return []
    segs = [s for s in _split_raw_segments(cmd) if s.strip()]
    if not segs:
        return []
    return [(s, is_codex_dispatch(s)) for s in segs]


# Shell grammar this splitter does NOT model. Each of these puts characters the
# scanner treats as structural into a context where they mean something else:
#   `#`      -- a comment can carry an unbalanced quote or a `<<WORD` that is
#               not a heredoc at all;
#   `$((`    -- `<<` inside it is a left-shift operator, not a redirection;
#   `case`   -- its patterns end in `)` with no opening paren, so a paren-
#               balancing scan of any enclosing substitution closes early.
# Every one was demonstrated to produce a WRONG split rather than a failed one,
# and a wrong split is worse: it yields confident segments that hide a bare
# suite run inside a fragment the caller then exempts. So they are declared
# unsupported and reported as unsplittable, which the caller treats as
# FAIL-CLOSED. Widening the splitter to model them properly means parsing shell
# with a real grammar, which is tracked separately -- until then, refusing to
# answer beats answering wrongly.
_UNMODELLED = (
    re.compile(r"(?:^|\s)#"),          # comment
    re.compile(r"\$\(\("),             # arithmetic expansion
    re.compile(r"(?:^|[\s;&|(])case(?=\s)"),
    re.compile(r"(?:^|[\s;&|(])esac(?=$|[\s;&|)])"),
)


def _has_unmodelled_grammar(cmd):
    """True when `cmd` uses shell grammar the raw splitter cannot model.

    Quote-aware: the same text inside a quoted review prompt is prose, and
    treating it as grammar would fail-closed on legitimate dispatches.
    """
    bare = []
    quote = None
    i, n = 0, len(cmd)
    while i < n:
        c = cmd[i]
        if c == "\\" and quote != "'" and i + 1 < n:
            i += 2
            continue
        if quote:
            if c == quote:
                quote = None
                bare.append(" ")
            i += 1
            continue
        if c in ("'", '"'):
            quote = c
            bare.append(" ")
            i += 1
            continue
        bare.append(c)
        i += 1
    text = "".join(bare)
    return any(rx.search(text) for rx in _UNMODELLED)


# Unquoted control operators that end one command and begin the next. `(` and
# `)` are deliberately NOT separators: a subshell's contents must stay attached
# to their own text so a caller sees `(bash foo.sh` with the paren intact.
_RAW_SEPS = (";;", "&&", "||", ";", "|", "&", "\n")

# A heredoc delimiter is a shell WORD, not an identifier: `<<'REVIEW-PROMPT'`
# and `<<"EOF.1"` are both valid and were missed by an identifier-only pattern,
# leaving their bodies to be scanned as commands. `<<<` is a here-STRING with no
# body and must not match at all.
_HEREDOC_START = re.compile(
    r"<<(?!<)(-?)\s*(?:'([^']*)'|\"([^\"]*)\"|([A-Za-z0-9_.\-]+))")


def _strip_heredoc_bodies(cmd):
    """Drop heredoc BODIES, keeping the command lines that introduce them.

    A heredoc body is DATA, not commands. The canonical apostrophe-safe dispatch
    shape puts the whole review prompt in one (`VAR=$(cat <<'X' ... X)`), so a
    prompt line that happens to read `bash scripts/test.sh fails because ...`
    would otherwise be split out as its own segment and classified as a bare
    suite invocation -- blocking the very review it describes.

    QUOTE-AWARE, because `<<` is only a redirection operator when it is shell
    SYNTAX. A review prompt that quotes heredoc syntax while discussing it is
    just text, and treating it as a real heredoc silently swallowed the rest of
    the prompt -- including its closing quote, which made the whole split fail
    and (under the fail-closed fallback) blocked a legitimate mandatory review.
    """
    out, pos, i, n = [], 0, 0, len(cmd)
    quote = None
    while i < n:
        c = cmd[i]
        if c == "\\" and quote != "'" and i + 1 < n:
            i += 2
            continue
        if quote:
            if c == quote:
                quote = None
            i += 1
            continue
        if c in ("'", '"'):
            quote = c
            i += 1
            continue
        m = _HEREDOC_START.match(cmd, i)
        if not m:
            i += 1
            continue
        tag = m.group(2) or m.group(3) or m.group(4)
        dash = m.group(1) == "-"
        nl = cmd.find("\n", m.end())
        if not tag or nl < 0:
            i = m.end()
            continue
        out.append(cmd[pos:nl])          # keep the introducing line
        k = nl + 1
        end = len(cmd)
        while k <= len(cmd):
            j = cmd.find("\n", k)
            line = cmd[k:] if j < 0 else cmd[k:j]
            if (line.lstrip() if dash else line).rstrip() == tag:
                end = len(cmd) if j < 0 else j
                break
            if j < 0:
                break
            k = j + 1
        pos = end
        i = end
    out.append(cmd[pos:])
    return "".join(out)


def _split_raw_segments(cmd):
    """Split on unquoted control operators, returning raw substrings.

    Quote- and escape-aware. Everything inside '...' is opaque, so a dispatch
    prompt containing `&&` or `;` is never split apart.

    Command substitutions -- `$(...)` and backticks -- are lifted out as their
    OWN segments, including inside double quotes, because they EXECUTE. A
    caller that exempts a whole fragment on the strength of the command it
    names would otherwise excuse everything nested in that fragment's
    arguments: `codex-dispatch.sh "$(bash scripts/test.sh)"` is one dispatch
    fragment, and the suite inside it runs regardless. Inside SINGLE quotes the
    same text is literal, so it stays attached to its fragment.
    """
    cmd = _strip_heredoc_bodies(cmd)
    out = []
    if not _scan_raw(cmd, out):
        return []
    return out


def _scan_raw(text, out):
    """Append `text`'s top-level segments to `out`; False on unbalanced quotes."""
    buf = []
    quote = None
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "\\" and quote != "'" and i + 1 < n:
            buf.append(c)
            buf.append(text[i + 1])
            i += 2
            continue
        if quote == "'":
            buf.append(c)
            if c == "'":
                quote = None
            i += 1
            continue
        if quote == '"':
            # Process substitutions are NOT expanded inside double quotes, so
            # only command substitutions are lifted here.
            if text.startswith("$(", i) or c == "`":
                i = _take_substitution(text, i, out)
                if i < 0:
                    return False
                buf.append(" ")
                continue
            buf.append(c)
            if c == '"':
                quote = None
            i += 1
            continue
        if c in ("'", '"'):
            quote = c
            buf.append(c)
            i += 1
            continue
        # `<(...)` / `>(...)` run their body CONCURRENTLY and are an argument to
        # the surrounding command, so an outer segment that earns an exemption
        # would otherwise carry them along: `codex-dispatch.sh <(bash
        # scripts/test.sh)` is one dispatch fragment that also runs the suite.
        if (text.startswith("$(", i) or c == "`"
                or text.startswith("<(", i) or text.startswith(">(", i)):
            i = _take_substitution(text, i, out)
            if i < 0:
                return False
            buf.append(" ")
            continue
        hit = ""
        for sep in _RAW_SEPS:
            if text.startswith(sep, i):
                hit = sep
                break
        if hit:
            out.append("".join(buf))
            buf = []
            i += len(hit)
            continue
        buf.append(c)
        i += 1
    if quote:
        # Unbalanced quotes: the split is not trustworthy, so report failure
        # and let the caller fall back to whole-command classification rather
        # than hand it segments that may have been cut mid-string.
        return False
    out.append("".join(buf))
    return True


def _take_substitution(text, i, out):
    """Scan the substitution starting at `i` into `out`; return the index just
    past it, or -1 if it is unterminated."""
    n = len(text)
    if (text.startswith("$(", i) or text.startswith("<(", i)
            or text.startswith(">(", i)):
        depth, q, j = 0, None, i + 2
        start = j
        while j < n:
            c = text[j]
            if c == "\\" and q != "'" and j + 1 < n:
                j += 2
                continue
            if q:
                if c == q:
                    q = None
                j += 1
                continue
            if c in ("'", '"'):
                q = c
                j += 1
                continue
            if c == "`":
                # A backtick substitution is its own lexical region, and its
                # contents may contain unbalanced parens: `echo )` is valid
                # shell. Scanning them as outer `$( )` delimiters closes the
                # substitution early, which desynchronises the rest of the walk
                # -- verified to make the whole split fail and hand the caller
                # a fail-open whole-command answer.
                k = text.find("`", j + 1)
                if k < 0:
                    return -1
                j = k + 1
                continue
            if c == "(":
                depth += 1
            elif c == ")":
                if depth == 0:
                    return j + 1 if _scan_raw(text[start:j], out) else -1
                depth -= 1
            j += 1
        return -1
    j = text.find("`", i + 1)
    if j < 0:
        return -1
    return j + 1 if _scan_raw(text[i + 1:j], out) else -1


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
