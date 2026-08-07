#!/usr/bin/env python3
"""Block accidental --model / --effort flags on Codex invocations.

PreToolUse hook on `Bash`. Reads the harness JSON from stdin, parses
the `tool_input.command` string, and exits 2 (block) when the
command:

  1. Targets Codex (mentions `codex-companion.mjs`, OR a token is
     `codex` followed by a subcommand, OR the command starts with
     `codex `), AND
  2. Contains a forbidden flag token: `--model`, `--effort`, or any
     value-attached form like `--model=gpt-5.5`, `--effort=high`,
     etc.

Per the impossible-os Codex invocation policy (CLAUDE.md "Model
Roles" -> "Codex Invocation Policy"): Claude must NEVER pass these
flags. Codex CLI uses `~/.codex/config.toml` defaults; the user
controls those values centrally.

Tokenization uses shlex so the false-positive-on-prompt-text case
(e.g. a prompt arg like "review whether --model gpt-5.5 is correct")
is only blocked if `--model` actually lands as its OWN cli token --
inside a quoted prompt string it stays embedded in one shlex token
that does NOT equal/start-with the flag literal.

Opt-out for legitimate cases (a deliberate one-off override the
user explicitly approves): set `CODEX_FLAG_OVERRIDE=1` on the
calling tool environment. The hook reads the env once and skips
the block.

Exits:
  0 -- allow (no Codex invocation, OR Codex with clean flags, OR
       opt-out env var set, OR malformed/missing input we cannot
       safely classify)
  2 -- block with stderr message naming the offending flag and the
       opt-out hint
"""

import json
import os
import shlex
import sys


FORBIDDEN_FLAG_PREFIXES = (
    "--model", "--effort",
    # Short forms per `codex exec --help`: -m for --model. Codex
    # currently has no -e short form for --effort but include it
    # forward-compat in case a future release adds one (no false
    # positive: -e is not a documented Codex flag today, so a
    # legitimate caller would not pass it).
    "-m", "-e",
)
# Token equality OR startswith("--flag=") matches both bare and
# `--flag=value` forms. Same for short forms (`-m gpt-5.5` or
# `-m=gpt-5.5`).

# Codex's `-c, --config <key=value>` lets callers override ANY
# config key, including model + reasoning effort. The bare `-c`
# flag is legitimate (sandbox, network, etc. overrides) -- block
# only when the value targets a forbidden config key.
CODEX_CONFIG_FLAGS = ("-c", "--config")
FORBIDDEN_CONFIG_KEYS = (
    "model", "model_reasoning_effort", "model_provider",
    # `model_provider` per `codex --help`; lets a caller swap the
    # provider implementation per-dispatch which undermines the
    # central-control policy as much as `model` does. (Codex
    # post-ship adversarial review M.)
)

# Bash control operators that introduce a NEW command sequence.
# Splitting on these lets us catch bypasses like
# `true && codex exec --model X`. Pipes, sequence semicolons,
# and short-circuit operators all qualify. (Codex post-ship
# adversarial review H.)
COMMAND_SEPARATORS = ("&&", "||", ";", "|", "&")

# Known command wrappers that prefix another command's argv.
# When tokens[0] is one of these, the actual command starts after
# the wrapper's own arguments. Strips so we can inspect the wrapped
# command (the real Codex invocation, if any). Hand-curated; common
# Linux wrappers only.
COMMAND_WRAPPERS = (
    "env", "timeout", "exec", "sudo", "nice", "ionice",
    "stdbuf", "nohup", "chrt", "taskset",
)

CODEX_SUBCOMMANDS = (
    # Per `codex --help`: bare subcommand names + documented aliases.
    "exec", "review", "task", "mcp", "app-server", "rescue",
    "exec-server", "resume", "fork", "apply",
    # Aliases: `codex e` -> exec, `codex a` -> apply. Adding these
    # closes the `codex e --model X` bypass. Edge case: an unquoted
    # prompt like `codex say e then a` would parse `e` and `a` as
    # subcommand candidates and treat the line as Codex; in practice
    # real prompts get quoted into one token so this false positive
    # almost never fires.
    "e", "a",
)

# Codex's global options that take a value (so when scanning past
# globals we can correctly skip the value too). Pulled from
# `codex --help` (top-level OPTIONS section). When a global flag
# takes a value, the value either follows as a separate token
# (`-c key=val`) or attaches via `=` (`-c=key=val`).
CODEX_GLOBAL_VALUE_FLAGS = (
    "-c", "--config", "--enable", "--disable", "--remote",
    "--bearer-token-env-var",
)


def _opt_out() -> bool:
    return os.environ.get("CODEX_FLAG_OVERRIDE", "") == "1"


def _is_env_assignment(tok: str) -> bool:
    """A leading shell env-assignment like `KEY=value`. Bash allows
    `KEY=value KEY2=value2 command args` and the command runs with
    those vars in its environment. We must skip these prefix tokens
    to find the real argv[0]. (Codex post-ship adversarial review
    H: `MODEL=foo codex exec --model X` bypassed because tokens[0]
    was `MODEL=foo`, not `codex`.)
    Valid identifier rules: starts with letter or `_`, then letters
    digits or `_`. Anything else (e.g. a flag like `--cwd=/tmp`)
    is NOT an env assignment."""
    if "=" not in tok or tok.startswith("-"):
        return False
    key = tok.split("=", 1)[0]
    if not key:
        return False
    if not (key[0].isalpha() or key[0] == "_"):
        return False
    return all(c.isalnum() or c == "_" for c in key)


def _segment_commands(tokens: list[str]) -> list[list[str]]:
    """Split tokens by Bash control operators (&&, ||, ;, |, &)
    into a list of simple commands. Each segment is its own argv.
    (Codex post-ship adversarial review H: `true && codex exec
    --model X` bypassed because the original code treated the
    whole command as one argv with `true` at position 0.)"""
    segs: list[list[str]] = []
    cur: list[str] = []
    for tok in tokens:
        if tok in COMMAND_SEPARATORS:
            if cur:
                segs.append(cur)
            cur = []
        else:
            cur.append(tok)
    if cur:
        segs.append(cur)
    return segs


def _strip_command_prefix(tokens: list[str]) -> list[str]:
    """Strip leading env-assignments and known wrappers (env,
    timeout, sudo, nice, ...) so the returned tokens start with the
    real command's argv[0]. (Codex post-ship adversarial review H.)"""
    i = 0
    # Phase 1: skip leading env-assignment prefix tokens.
    while i < len(tokens) and _is_env_assignment(tokens[i]):
        i += 1
    if i >= len(tokens):
        return []
    # Phase 2: if argv[0] is now a known wrapper, skip past it AND
    # its own arguments to reach the wrapped command. Wrapper-argument
    # patterns we recognize:
    #   * env [-i] [KEY=val ...] [-u KEY ...] command args
    #   * timeout [OPT...] DURATION command args
    #   * sudo [OPT...] [-E] [-u USER] command args
    #   * nice / ionice / nohup / chrt / taskset: similar OPT...
    # Heuristic: skip flag tokens (`-...`, `--...`), env-assignment
    # tokens, and pure-numeric tokens (timeout's DURATION argument)
    # until the next bare-word token.
    base = os.path.basename(tokens[i].rstrip("/"))
    if base in COMMAND_WRAPPERS:
        i += 1
        while i < len(tokens):
            t = tokens[i]
            if t.startswith("-"):
                i += 1
                continue
            if _is_env_assignment(t):
                i += 1
                continue
            # Pure-numeric (timeout's seconds arg, nice's level).
            try:
                float(t)
                i += 1
                continue
            except ValueError:
                pass
            break
    return tokens[i:]


def _trim_heredoc_body(tokens: list[str]) -> list[str]:
    """Return tokens up to (but not including) the first construct
    that introduces SHELL SUB-CONTENT (heredoc body, process
    substitution body, command substitution body). Tokens after
    the opener are NOT part of the real command line -- shlex
    flattens them but they belong to a separate execution context
    (the heredoc body, the inner $(...) command, etc.).

    Constructs trimmed:
      `<<` / `<<-` / `<<DELIM` -- heredoc body follows.
      `<(...)` / `>(...)` -- process substitution body in token.
      `$(...)` / starts with `$(` -- command substitution body
        in token. shlex tokenizes `git commit -m "$(cat <<EOF
        ... EOF)"` such that the `$(cat` token contains the open
        parenthesis; the message body words (including any
        forbidden-flag literals) follow as separate tokens. The
        previous revision missed this and false-positive-blocked
        the very git commit that landed the post-ship review
        fixes (the commit message body mentioned `-m`, `--model`,
        and the literal Codex-subcommand strings as text inside
        the heredoc fed via $(cat <<EOF ... EOF)).
      `${...}` / starts with `${` -- parameter expansion with
        sub-content (rare but same family).
      Plain file redirects (`<`, `>`, `>>`, `<>`) do NOT trim --
        their target path is harmless to scan.

    Returns tokens unchanged if no marker is found.
    """
    for i, tok in enumerate(tokens):
        if tok in ("<<", "<<-") or tok.startswith("<<"):
            return tokens[:i]
        if tok.startswith("<(") or tok.startswith(">("):
            return tokens[:i]
        if tok.startswith("$(") or tok.startswith("${"):
            return tokens[:i]
        # Backtick command substitution: `cmd args` -- shlex with
        # posix=True keeps the backticks as part of the token.
        if tok.startswith("`"):
            return tokens[:i]
    return tokens


# Commands that take a following argument as DATA. If one of these heads the
# segment, a later `codex-companion.mjs` token is text -- a commit message, a
# search pattern, an echoed doc reference -- not an invocation.
#
# A DISQUALIFIER list, deliberately, not a validator of everything before the
# mention. Validating forward re-introduces exactly the fragility this module's
# docstring records paying for twice: it falls behind on wrapper VALUE tokens
# like sudo's `-u root` and timeout's `5s`, which are neither flags nor
# wrappers. Blocking on a known consumer is stable because the list only has to
# name things that CONSUME text, and a miss fails toward BLOCKING, which is the
# safe direction for this gate.
_DATA_CONSUMERS = frozenset({
    "git", "echo", "printf", "rg", "grep", "egrep", "fgrep", "ag", "ack",
    "cat", "sed", "awk", "tee", "less", "more", "head", "tail", "jq",
    "python", "python3", "perl", "ruby", "diff", "comm", "sort", "uniq",
})


def _runs_it(tokens: list[str], idx: int) -> bool:
    """False when the segment is headed by a command that consumes text.

    `git commit -m "fixed codex-companion.mjs dispatch"` was blocked as a Codex
    invocation carrying a model flag: shlex keeps the quoted message as ONE
    token, the scan matched the companion path inside it, and `-m` is git's.
    Hit live 2026-08-07 while committing the v10 fixes. The documented heredoc
    guard does not cover a plain quoted argument.
    """
    for t in tokens[:idx]:
        if os.path.basename(t.rstrip("/")) in _DATA_CONSUMERS:
            return False
    return True


def _command_argv(tokens: list[str]) -> list[str] | None:
    """Return the segment's tokens if it contains a Codex invocation,
    else None.

    Scans ALL tokens for either (a) a `codex` token (basename match)
    followed by a Codex subcommand, OR (b) any token containing
    `codex-companion.mjs`. This handles wrappers + env-prefixes
    uniformly: `sudo -u root codex exec --model X`,
    `MODEL=foo codex exec --model X`, `timeout 5s codex exec
    --model X`, `nice -n 10 codex exec --model X`, etc. all surface
    via the same scan. Earlier revisions tried to strip wrappers
    one-by-one and fell behind on `-u root` (sudo's value-taking
    flag) and `5s` (timeout's duration-with-suffix); the
    scan-anywhere approach has no such omission.

    False-positive concern: a python heredoc body that happens to
    contain `codex exec` literal text would be flagged. Closed by
    the heredoc / process-substitution fail-open guard at the top
    of main() -- if any token equals/startswith `<<` or starts with
    `<(`/`>(`, main() returns 0 BEFORE this function runs.
    """
    if not tokens:
        return None
    for i, tok in enumerate(tokens):
        base = os.path.basename(tok.rstrip("/"))
        if base == "codex":
            # Walk forward past Codex global options to find the
            # subcommand. (Codex global options that take a value
            # need both their flag and value tokens skipped.)
            j = i + 1
            while j < len(tokens):
                t = tokens[j]
                if t in CODEX_SUBCOMMANDS:
                    return tokens
                if t in CODEX_GLOBAL_VALUE_FLAGS:
                    j += 2
                    continue
                if any(t.startswith(g + "=") for g in CODEX_GLOBAL_VALUE_FLAGS):
                    j += 1
                    continue
                if t.startswith("-"):
                    j += 1
                    continue
                # Bare positional that isn't a subcommand: not a
                # Codex CLI invocation (looks like bare `codex`
                # interactive TUI receiving a prompt as positional).
                return None
            return None  # `codex` token but no subcommand follows
        # COMMAND POSITION, not anywhere. A token merely CONTAINING the
        # companion path is an ARGUMENT unless the segment actually runs it --
        # and `git commit -m "fixed codex-companion.mjs dispatch"` was blocked
        # as a Codex invocation carrying a model flag, because shlex keeps the
        # quoted message as one token and `-m` is git's. Hit live 2026-08-07
        # while committing the v10 fixes; the documented heredoc guard does not
        # cover a plain quoted argument.
        #
        # The real shapes all put the companion at the head, after at most an
        # interpreter and wrappers: `node .../codex-companion.mjs ...` and
        # `.../codex-companion.mjs ...`. So the mention counts only when every
        # token before it is an env-prefix, a wrapper, or an interpreter.
        if "codex-companion.mjs" in tok and _runs_it(tokens, i):
            return tokens
    return None


def _forbidden_flag_token(tok: str) -> str | None:
    """If `tok` is a forbidden flag (bare or `--flag=value` form),
    return the canonical flag name; else None."""
    for prefix in FORBIDDEN_FLAG_PREFIXES:
        if tok == prefix or tok.startswith(prefix + "="):
            return prefix
    return None


def _emit_block(flag: str, raw_command: str) -> None:
    sys.stderr.write(
        f"[codex-flag-block] BLOCK -- forbidden flag {flag!r} on a "
        f"Codex invocation.\n"
        f"[codex-flag-block] policy: Claude must NEVER pass --model "
        f"or --effort to any Codex command (codex-companion.mjs, "
        f"codex exec, codex review, etc.). Codex CLI uses the "
        f"configured defaults from ~/.codex/config.toml; the user "
        f"controls those values centrally.\n"
        f"[codex-flag-block] hint: drop the {flag} flag and re-run. "
        f"For a one-off override (rare), set CODEX_FLAG_OVERRIDE=1 "
        f"on the same call.\n"
        f"[codex-flag-block] command: {raw_command[:200]}"
        f"{'...' if len(raw_command) > 200 else ''}\n"
        f"[codex-flag-block] doctrine: CLAUDE.md \"Model Roles\" -> "
        f"\"Codex Invocation Policy\".\n"
    )


def main() -> int:
    if _opt_out():
        return 0
    try:
        payload = json.load(sys.stdin)
    except Exception:
        # Malformed input -- fail open. The harness will surface its
        # own malformed-payload error; we don't want a hook crash to
        # block unrelated tool calls.
        return 0
    if payload.get("tool_name") != "Bash":
        return 0
    cmd = (payload.get("tool_input") or {}).get("command", "")
    if not isinstance(cmd, str) or not cmd.strip():
        return 0
    # Space-pad unquoted control operators BEFORE tokenizing.
    #
    # shlex.split only breaks on whitespace, so an operator written without
    # surrounding spaces stays glued to its neighbour and the segment walk below
    # never sees it. Measured 2026-07-27: `true&&codex exec --model gpt-4` and
    # `cd /tmp;codex exec --model gpt-4` were ALLOWED while the spaced forms were
    # BLOCKED -- a real bypass of this policy, in a hook whose entire job is to
    # be un-bypassable. It is the same substring-vs-argv weakness the module
    # docstring already describes, one level lower down.
    #
    # Reuses the shared quote-aware padder so this hook and the dispatch
    # recognizer cannot drift: an operator inside a quoted prompt is never
    # touched, and redirects (`2>&1`, `&>log`) are left intact so the
    # complex-construct fail-open below still triggers on exactly what it did.
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from _codex_dispatch import _unquoted_newlines_to_sep as _pad_ops
        cmd_for_tokens = _pad_ops(cmd)
    except Exception:  # noqa: BLE001 -- padding is a hardening step, never fatal
        cmd_for_tokens = cmd
    try:
        tokens = shlex.split(cmd_for_tokens, posix=True)
    except ValueError:
        # Unparseable shell command (unbalanced quotes); fail open.
        # A real shell would reject it too; we don't want to mask
        # that error with a hook block.
        return 0
    # Fail-open on commands containing complex shell constructs
    # (heredocs, file redirects, process substitution) -- shlex
    # flattens those into ordinary tokens, which means a heredoc
    # body containing `&& codex exec --model X` literal text would
    # otherwise look like a real Codex invocation after segmentation.
    # We only enforce on simple-shape commands; complex shell goes
    # to human review. (Caught while adding bypass tests for
    # post-ship adversarial review H findings -- the test-adding
    # python heredoc itself contained the literal text and got
    # blocked.)
    # Segment by control operators so `true && codex exec --model X`
    # is checked at the codex segment, not just at the first command.
    # (Codex post-ship adversarial review H.)
    for segment in _segment_commands(tokens):
        # Trim heredoc / process-substitution body tokens from the
        # segment END before classifying. The command tokens BEFORE
        # the heredoc marker are still scannable; only the body is
        # ambiguous. Closes the `codex exec --model X <<EOF body EOF`
        # bypass the consistency review caught -- the previous
        # revision did a top-level fail-open that allowed this.
        cmd_part = _trim_heredoc_body(segment)
        # _command_argv scans for a `codex` token + subcommand or
        # for codex-companion.mjs, so we don't need wrapper-specific
        # parsing -- handles `sudo -u root codex exec --model X`,
        # `MODEL=foo codex exec --model X`, `timeout 5s codex exec
        # --model X` etc. uniformly.
        codex_argv = _command_argv(cmd_part)
        if codex_argv is None:
            continue
        if _scan_codex_segment(codex_argv, cmd) == 2:
            return 2
    return 0


def _scan_codex_segment(codex_argv: list[str], raw_cmd: str) -> int:
    """Scan one Codex argv for forbidden flag tokens AND forbidden
    `-c key=value` config overrides. Returns 2 (block + emit) on
    hit, 0 on clean."""
    i = 0
    while i < len(codex_argv):
        tok = codex_argv[i]
        hit = _forbidden_flag_token(tok)
        if hit is not None:
            _emit_block(hit, raw_cmd)
            return 2
        # `-c <key=value>` / `--config <key=value>` separated form.
        if tok in CODEX_CONFIG_FLAGS and i + 1 < len(codex_argv):
            val = codex_argv[i + 1]
            for key in FORBIDDEN_CONFIG_KEYS:
                if val == key or val.startswith(key + "="):
                    _emit_block(f"{tok} {key}=...", raw_cmd)
                    return 2
            i += 2
            continue
        # `-c=<key=value>` / `--config=<key=value>` attached form.
        for cfg in CODEX_CONFIG_FLAGS:
            if tok.startswith(cfg + "="):
                val = tok[len(cfg) + 1:]
                for key in FORBIDDEN_CONFIG_KEYS:
                    if val == key or val.startswith(key + "="):
                        _emit_block(f"{cfg}={key}=...", raw_cmd)
                        return 2
        i += 1
    return 0  # clean segment


if __name__ == "__main__":
    sys.exit(main())
