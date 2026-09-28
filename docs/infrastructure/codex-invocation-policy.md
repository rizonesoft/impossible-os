<!-- docs: covers=todo/00-infrastructure/TODO-08-automation-hardening.md sources=scripts/test-tooling.sh reviewed=2026-09-28T15:18 -->
# Codex Invocation Policy

> When Claude invokes Codex from any path, Claude does NOT pass any flag that overrides the centrally-configured Codex model or reasoning effort. The user controls those values centrally via `~/.codex/config.toml`. The `model` value is pinned by `docs/infrastructure/codex-config-policy.toml` and audited (currently `model = "gpt-5.6-sol"`); the `model_reasoning_effort` value is a user dial (low / medium / high all legitimate) and is NOT pinned or audited. CLAUDE.md "Model Roles" carries the rule; the enforcement mechanics live here.

## Rule

When Claude invokes Codex from any path -- the in-repo `codex-*` skills under [`.claude/skills/`](../../.claude/skills/), the Codex plugin slash commands, raw `node ...codex-companion.mjs ...` calls, or `codex exec` / `codex review` / `codex task` -- Claude does NOT pass any flag that overrides the centrally-configured Codex model or reasoning effort. The user controls those values centrally; Claude must not second-guess per-dispatch.

## Enforcement

A PreToolUse hook ([`.claude/hooks/codex_model_flag_block.py`](../../.claude/hooks/codex_model_flag_block.py)) parses every `Bash` tool call's command via `shlex`, segments by Bash control operators (`&&` / `||` / `;` / `|` / `&`), trims heredoc / process-substitution body tokens from each segment, then detects Codex invocations.

### Codex invocation detection

Any of:

- A token containing `codex-companion.mjs`.
- A `codex` token (bare or path-prefixed) followed by a known subcommand: `exec` / `review` / `task` / `mcp` / `app-server` / `rescue` / `exec-server` / `resume` / `fork` / `apply`, or aliases `e` / `a`.

### Forbidden flags / overrides

Hook exits 2 (block) when ANY of the following appears in a Codex segment:

- Long flags: `--model`, `--effort` (bare or `--flag=value` attached form).
- Short flags: `-m` (Codex's documented short for `--model`), `-e` (forward-compat for a future short of `--effort`).
- Config-key overrides via `-c` / `--config <key=value>`: `model`, `model_reasoning_effort`, `model_provider` (the keys that materially redirect Codex to a different model selection per dispatch). `-c sandbox_mode=...` and other non-forbidden config keys remain legitimate.

### Bypass shapes handled uniformly

- Env-assignment prefixes (`MODEL=x codex exec ...`).
- Wrapper commands (`sudo`, `timeout`, `nice`, `env`, `nohup`, `taskset`, `chrt`, `setsid`, `setpriv`, `cgexec`, `flock`, etc.).
- `&&` / `||` / `;` / `|` chains.
- Global Codex options BEFORE the subcommand (`codex --json exec ...`).
- Heredoc / process-substitution INPUT to a Codex command (the heredoc body is dropped but the command tokens before it are still scanned).

Prompt text that happens to contain a flag literal (e.g. `codex exec "review whether --model gpt-5.5 is right"`) is NOT blocked because shlex tokenization keeps that text inside its containing quoted-arg token.

## Opt-out

For legitimate one-off overrides: `CODEX_FLAG_OVERRIDE=1` env var on the same call. The agent must set it explicitly per call; the user sees it in the diff.

## Test coverage

29-case `codex_flag_block` sub-test in [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh) covering every block path + every allow path + both bypass-regression cases identified by Codex post-ship adversarial review (env-prefix, wrapper, chain, redirect, heredoc-on-Codex).

## Note on absolute paths

In-repo skills invoke Codex via the absolute path `node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs"`. The Codex plugin's own slash-command files use `${CLAUDE_PLUGIN_ROOT}` substitution, but that variable is **not exported** in the harness env that runs Claude skill / hook / Bash tool calls (verified via `env | grep CLAUDE` -- only `CLAUDECODE`, `CLAUDE_CODE_SSE_PORT`, `CLAUDE_CODE_ENTRYPOINT`, `CLAUDE_CODE_EXECPATH` are present). The absolute path is the canonical form for skill-driven Codex invocation today; consistent across 15 skill files. Re-evaluate if Claude Code starts exporting `CLAUDE_PLUGIN_ROOT` to skill / hook execution contexts.
