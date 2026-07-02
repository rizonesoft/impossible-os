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

# Review-carrying bare CLI subcommands ONLY. `codex e` / `codex exec` are
# non-review automation (see docs/infrastructure/ai-driver-interchangeability.md):
# trusting the `e` alias let a mutating session mint reviewer evidence by
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
            second.endswith("codex-dispatch.sh"):
        return True
    if head_base == "codex-dispatch.sh" or head_base.endswith("/codex-dispatch.sh"):
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
                out[i + 1].endswith("codex-dispatch.sh"):
            return _first_positional_after(out, i + 2)
        if base == "codex-dispatch.sh" or base.endswith("/codex-dispatch.sh"):
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
    if not isinstance(cmd, str) or not cmd.strip():
        return ""
    if "codex" not in cmd:
        return ""
    stripped, heredoc_vars = _harvest_heredoc_vars(cmd)
    toks = _tokenize(stripped)
    if not toks:
        return ""
    for seg in _segment_by_separators(toks):
        seg = _trim_heredoc_body(seg)
        if seg and _segment_is_codex_invocation(seg):
            prompt = _segment_extract_prompt(seg)
            return _resolve_var(prompt, heredoc_vars)
    return ""
