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
FORBIDDEN_CONFIG_KEYS = ("model", "model_reasoning_effort")

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


def _command_argv(tokens: list[str]) -> list[str] | None:
    """Return the argv of the actual Codex invocation when one is the
    main command being executed by this shell line, else None.

    We MUST distinguish "Codex is the main command" from "this command
    happens to mention `codex` somewhere deeper in its argv" -- e.g. a
    `python3 <<'EOF' ... --model gpt-5.5 ... EOF` heredoc would have
    `--model` as a standalone token but `python3` as argv[0]; that is
    NOT a Codex invocation and must not be blocked. The earlier
    revision scanned ALL tokens which produced exactly that false
    positive (the heredoc body of an unrelated python3 script).

    Detection rule, in order of preference:
      1. argv[0] basename == "codex" AND argv[1] is in CODEX_SUBCOMMANDS
         -> return tokens (Codex CLI invocation).
      2. argv[0] basename in {"node", "nodejs"} AND any of argv[1..]
         until the first non-flag/non-path token contains
         `codex-companion.mjs`
         -> return tokens (codex-companion.mjs script invocation).
         Allows `node --inspect /path/to/codex-companion.mjs ...`
         and `node /path/to/codex-companion.mjs subcommand ...`.

    Anything else returns None and the hook exits 0. Compound shell
    commands (heredocs, pipes, &&) tokenize at the shell level; only
    the FIRST sub-command's argv matters here -- if a heredoc's
    payload happens to look like a Codex command, that's the
    payload's responsibility (and won't reach the harness's Bash
    tool wrapper as a separate call)."""
    if not tokens:
        return None
    cmd0 = tokens[0]
    base = os.path.basename(cmd0.rstrip("/"))
    if base == "codex":
        # `codex [OPTIONS] <COMMAND> [ARGS]` per docs. Walk past
        # global options (which may carry their own values either
        # attached via `=` or as the next token) until we find a
        # token that is a Codex subcommand. Earlier revision only
        # checked tokens[1] which let `codex --json exec --model X`
        # bypass (Codex post-impl adversarial review High).
        i = 1
        while i < len(tokens):
            tok = tokens[i]
            if tok in CODEX_SUBCOMMANDS:
                return tokens
            # Global option with separate value -- skip both tokens.
            if tok in CODEX_GLOBAL_VALUE_FLAGS:
                i += 2
                continue
            # Attached form (`--config=key=val` or `-c=key=val`).
            if any(tok.startswith(g + "=") for g in CODEX_GLOBAL_VALUE_FLAGS):
                i += 1
                continue
            # Any other token starting with `-` is a boolean flag
            # (--json, --quiet, etc.) -- skip just this token.
            if tok.startswith("-"):
                i += 1
                continue
            # First positional that isn't a subcommand: this is
            # being used as a prompt to the bare `codex` (TUI auto-
            # forwards options + prompt). Not a target.
            return None
        return None
    if base in ("node", "nodejs"):
        # Walk past node's own flags to find the script path.
        for tok in tokens[1:]:
            if tok.startswith("--"):
                # A node flag (--inspect, --max-old-space-size=, etc.)
                # -- skip and keep looking.
                continue
            if "codex-companion.mjs" in tok:
                return tokens
            # First positional that isn't codex-companion.mjs --
            # this is some other script; not a Codex invocation.
            return None
        return None
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
    try:
        tokens = shlex.split(cmd, posix=True)
    except ValueError:
        # Unparseable shell command (unbalanced quotes); fail open.
        # A real shell would reject it too; we don't want to mask
        # that error with a hook block.
        return 0
    codex_argv = _command_argv(tokens)
    if codex_argv is None:
        return 0
    # Walk argv looking for forbidden flag tokens AND forbidden
    # `-c key=value` config overrides (Codex post-impl adversarial
    # review Medium: `-c model="x"` sets the model centrally too,
    # bypassing the long-flag-only check).
    i = 0
    while i < len(codex_argv):
        tok = codex_argv[i]
        hit = _forbidden_flag_token(tok)
        if hit is not None:
            _emit_block(hit, cmd)
            return 2
        # `-c <key=value>` / `--config <key=value>` separated form.
        if tok in CODEX_CONFIG_FLAGS and i + 1 < len(codex_argv):
            val = codex_argv[i + 1]
            for key in FORBIDDEN_CONFIG_KEYS:
                if val == key or val.startswith(key + "="):
                    _emit_block(f"{tok} {key}=...", cmd)
                    return 2
            i += 2
            continue
        # `-c=<key=value>` / `--config=<key=value>` attached form.
        for cfg in CODEX_CONFIG_FLAGS:
            if tok.startswith(cfg + "="):
                val = tok[len(cfg) + 1:]
                for key in FORBIDDEN_CONFIG_KEYS:
                    if val == key or val.startswith(key + "="):
                        _emit_block(f"{cfg}={key}=...", cmd)
                        return 2
        i += 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
