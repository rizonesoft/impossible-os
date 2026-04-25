---
schema_version: 1
id: automation-hardening
domain: 00-infrastructure
status: draft
title: "TODO-08 -- Automation Hardening (Skill / Hook / MCP / Codex Integration)"
---

# TODO-08 -- Automation Hardening (Skill / Hook / MCP / Codex Integration)

> **Goal:** Take the existing Claude Code + Codex + LSP-MCP + todo-graph automation surface from "works most of the time" to "works on every single section, every single review, every single commit". Wire the two MCP servers into Codex (Claude Code already has them via `.mcp.json`); enforce the model-and-effort defaults policy across both sides; convert today's advisory hook reminders into hard gates where critical steps keep getting skipped (Codex review, `receiving-code-review`, `review-todo-section` step 8 quality dispatch); and remove the cross-tool drift between `.claude/`, `~/.codex/`, and the two installed plugins (superpowers 5.0.7, openai-codex 1.0.2).

> [!IMPORTANT]
> **Current state:**
> - **Claude Code MCP wiring** at [`.mcp.json`](../../.mcp.json) loads two servers: `todo-graph` (read-only graph queries over [`scripts/todo-graph/mcp_server.py`](../../scripts/todo-graph/mcp_server.py), TODO-06 §8) and `lsp-bridge` (read-only LSP-over-MCP via [`scripts/lsp-mcp/bridge.py`](../../scripts/lsp-mcp/bridge.py), TODO-07).
> - **Codex CLI MCP wiring** is empty: [`~/.codex/config.toml`](file:///home/derickpayne/.codex/config.toml) has only `model = "gpt-5.5"`, `model_reasoning_effort = "medium"`, the trust block, and the gpt-5.3-codex -> gpt-5.4 migration alias. No `[mcp_servers.*]` blocks. Codex CLI 0.125.0+ supports `codex mcp add` (per `codex --help`), so the wiring is available; just not used.
> - **Plugins installed** (per [`installed_plugins.json`](file:///home/derickpayne/.claude/plugins/installed_plugins.json)): `superpowers@claude-plugins-official` 5.0.7 (user scope, 12 skills incl. `receiving-code-review`, `requesting-code-review`, `verification-before-completion`, `writing-plans`, `using-git-worktrees`); `codex@openai-codex` 1.0.2 (user scope, skills `codex-cli-runtime` / `codex-result-handling` / `gpt-5-4-prompting`, commands `/codex:review` `/codex:adversarial-review` `/codex:setup` `/codex:status` `/codex:result` `/codex:cancel` `/codex:rescue`, agent `codex-rescue`, hooks shipped). Both plugin caches under [`~/.claude/plugins/cache/`](file:///home/derickpayne/.claude/plugins/cache/).
> - **Hook system** at [`.claude/settings.json`](../../.claude/settings.json) + [`.claude/hooks/`](../../.claude/hooks/) is mostly reminder-based (`systemMessage` print) with 3 hard blocks (Unicode dashes exit 2, bare section refs at commit, accepted-XREF block). The big-impact reminders (`section_review_required.py`, `design_review_required.py`, the section-commit gate, the receiving-code-review nudge after Codex finishes) all rely on the agent reading the hook output and acting; user feedback memory `feedback_skill_invocation_drift.md` and `feedback_never_skip_review.md` confirm this is the single biggest reliability gap.
> - **codex-companion.mjs** dispatch from Claude into Codex CLI is the only way the Codex plugin runs reviews; the companion accepts `--model` and `--effort` flags but the user's policy is "leave both unset, Codex CLI uses the default in `~/.codex/config.toml`". The 9 in-repo `codex-*` skills under [`.claude/skills/`](../../.claude/skills/) all dispatch to `codex-companion.mjs` correctly, but several pre-1.0.2 docs/skills still hard-code the absolute marketplaces path; one such absolute-path invocation is currently in `codex-adversarial-review-section/SKILL.md` step 3.
> - **Receiving-code-review** is wired into the 9 codex-* skills as prose ("apply receiving-code-review discipline") and as a PostToolUse `systemMessage` hook on `Bash(scripts/copilot-review.sh:*)` and `Bash(codex-companion.mjs adversarial-review:*)`. There is no hook that fires after a `Skill(codex-adversarial-review-section)` completes, no audit that the receiving-code-review skill was actually invoked between the Codex output and the next code edit, and no marker file or recent-conversation check.
> - **Skipped-step incidents** in user feedback memory: 2026-04-24 (review-todo-section step 8 = adversarial conflated with quality dispatches; user fed back four-dispatch policy), 2026-04-07 (3 incidents of tests calling forbidden boot fns -> hook added), repeated drift on `kernel-code-quality` not being walked. The pattern: hooks remind, agent acknowledges, agent skips. Hard blocks on the *next tool call after a trigger* are the only reliable fix.
> - **No Codex-side MCP** = Codex CLI cannot query the TODO graph or hover/definition-lookup symbols when running adversarial review or rescue tasks. Every Codex review reads files cold, missing the dependency context Claude has via the MCP servers.

## Inputs

- [`.claude/settings.json`](../../.claude/settings.json) -- canonical hook + permissions config; this TODO edits `hooks.PreToolUse` / `hooks.PostToolUse` blocks
- [`.claude/hooks/`](../../.claude/hooks/) -- Python hook scripts: `section_review_required.py`, `design_review_required.py`, `accepted_xref_block.py`, `bare_section_refs.py`, `numeric_todo_shorthand.py`, `notes_bloat_check.py`, `scope_gap_marker.py`, `scope_gap_dedup_check.py`, `test_pending_reminder.py`, `test_message_uniqueness.py`, `test_side_effect_ban.py`, `todo_format_check.py`, `accepted_xref_warn.py` -- §3 + §4 add 4-6 new ones
- [`.mcp.json`](../../.mcp.json) -- Claude Code MCP manifest; §1 mirrors this into Codex side
- [`~/.codex/config.toml`](file:///home/derickpayne/.codex/config.toml) -- Codex CLI config; §1 adds `[mcp_servers.todo-graph]` + `[mcp_servers.lsp-bridge]` blocks; §2 documents the model/effort policy
- [`scripts/todo-graph/mcp_server.py`](../../scripts/todo-graph/mcp_server.py) -- existing FastMCP server (TODO-06 §8); §1 confirms it works under Codex's MCP client
- [`scripts/lsp-mcp/bridge.py`](../../scripts/lsp-mcp/bridge.py) -- existing FastMCP server (TODO-07); §1 confirms cross-client and §6 wires usage discipline
- [`scripts/codex-companion.mjs`](../../scripts/codex-companion.mjs) -- absent in repo; canonical copy lives in plugin at `~/.claude/plugins/cache/openai-codex/codex/1.0.2/scripts/codex-companion.mjs`. §2 verifies the absolute-path versus `${CLAUDE_PLUGIN_ROOT}` invocation across all 9 codex-* skills
- [`.claude/skills/`](../../.claude/skills/) -- 28 skills (11 `codex-*`, plus implement / verify / review / quality / create / validate); §5 audits invocation discipline
- [`~/.claude/plugins/cache/claude-plugins-official/superpowers/5.0.7/skills/`](file:///home/derickpayne/.claude/plugins/cache/claude-plugins-official/superpowers/5.0.7/skills/) -- 12 superpowers skills; §7 audits which are actually useful in Impossible OS context and which to suppress
- [`~/.claude/plugins/cache/openai-codex/codex/1.0.2/`](file:///home/derickpayne/.claude/plugins/cache/openai-codex/codex/1.0.2/) -- Codex plugin: `commands/adversarial-review.md`, `agents/codex-rescue.md`, `skills/codex-cli-runtime`, `hooks/hooks.json`
- [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) -- doctrine doc; §8 updates the External-Reviewer Contract + Hook Routing Matrix sections
- [`AGENTS.md`](../../AGENTS.md) -- pointer file for non-Claude tools; §8 verifies the 3-bullet authority block stays exact-match
- [`CLAUDE.md`](../../CLAUDE.md) -- master doctrine; §2 adds a one-paragraph "Codex invocation policy" section under Model Roles
- -> XREF: [`00-infrastructure/TODO-02 §3 Hook Routing Matrix`](TODO-02-ai-development-system.md) -- §3 + §4 add new hook rows, this TODO is the implementation owner; TODO-02 is the documentation owner
- -> XREF: [`00-infrastructure/TODO-02 §4 External-Reviewer Contract`](TODO-02-ai-development-system.md) -- §5 enforces the contract via tool-call gating; TODO-02 owns the doctrine paragraph
- -> XREF: [`00-infrastructure/TODO-06 §8 todo-graph MCP server`](TODO-06-todo-metadata-layer.md#8-mcp-server-read-only-ai-agent-transport-over-4) -- consumer; §1 + §6 specify when Claude AND Codex must call which MCP method
- -> XREF: [`00-infrastructure/TODO-07 §1, §7, §8`](TODO-07-lsp-mcp-bridge.md) -- consumer; §1 wires the bridge into Codex; §6 wires usage discipline into both sides

## Outcome

- **Codex CLI loads the same two MCP servers as Claude Code.** `~/.codex/config.toml` has `[mcp_servers.todo-graph]` and `[mcp_servers.lsp-bridge]` blocks pointing at the in-repo Python entry points; a sentinel test (`scripts/test-tooling.sh` adds a sub-test) confirms `codex mcp list` shows both servers and a smoke task can reach them. Drift between Claude `.mcp.json` and Codex MCP config is detected by a new validator in `scripts/test-tooling.sh`.
- **Codex invocation policy is documented and enforced.** A new "Codex Invocation Policy" paragraph in CLAUDE.md (under Model Roles) states: when invoking ANY Codex command from Claude (`/codex:adversarial-review`, `/codex:review`, `/codex:rescue`, direct `codex-companion.mjs task` calls), do NOT pass `--model` or `--effort`; let Codex CLI pick its configured defaults from `~/.codex/config.toml`. A new hook (`.claude/hooks/codex_model_flag_block.py`) BLOCKS Bash invocations that include `--model` or `--effort` against `codex-companion.mjs` or `codex` binary unless the user explicitly added an opt-out env var. The 9 `codex-*` skills under `.claude/skills/` and the Codex plugin docs are checked for any leftover hard-coded model/effort hints.
- **`receiving-code-review` is enforced as a hard gate.** After ANY tool invocation that returns Codex/Copilot review output (Skill calls to `codex-adversarial-review-section`, `codex-review-todo`, `codex-fix-review`, `codex-design-review`, `codex-test-coverage`, `codex-impact-analysis`, `codex-consistency-audit`, `codex-perf-review`, `codex-quality-review-section`, OR Bash calls to `codex-companion.mjs adversarial-review` / `scripts/copilot-review.sh` / `/codex:adversarial-review`), the next Edit / Write / MultiEdit on a code file is BLOCKED until the conversation contains an explicit `Skill(superpowers:receiving-code-review, ...)` invocation OR an opt-out marker (the agent claimed the review found zero findings, with code-evidence quote required by the hook). Implemented via a state file at `.claude/state/last-codex-review.json` and a new `.claude/hooks/receiving_review_required.py`.
- **Section-commit gate hardens to a BLOCK, not just a reminder.** The current `git commit` PreToolUse hook prints a long advisory listing steps 13-18; it does not block. New behavior: when the staged diff contains both source code (`.c`/`.h`/`.asm`/`.S`) AND a TODO file with an Implementation Order `[x]` flip in the same diff, the hook EXITS 2 (block) unless the recent conversation contains evidence of (a) a `Skill(codex-adversarial-review-section, ...)` invocation, (b) a `Skill(superpowers:receiving-code-review, ...)` invocation, AND (c) a passing build (`tail -1 build/build.log` shows `=== BUILD OK ===`). Same `SKIP_REVIEW_HOOK=1` opt-out as today, plus a new `SKIP_REVIEW_HOOK_REASON="<text>"` requirement to leave a paper trail.
- **`review-todo-section` step 8 four-dispatch policy is mechanically enforced.** A post-step-8 marker file (`.claude/state/last-review-stamps.json`) records each of the 4 required Codex dispatches (adversarial / consistency / dead-code / perf); the section-commit hook checks all 4 are present-and-recent (within the last 30 minutes for the same TODO file path). The user's feedback memory `feedback_codex_review_four_dispatches.md` becomes machine-checkable.
- **Hook system audit + dedupe.** All 14 active hooks under `.claude/hooks/` + the inline Python in `settings.json` get a 1-line description and a coverage note; hooks doing the same job collapse to one. New `scripts/audit-hooks.sh` lists every hook, every matcher, and the trigger event; `bash scripts/test-tooling.sh` adds a sub-test that asserts the hook count and coverage matches an expected manifest at `.claude/hooks/MANIFEST.md`.
- **Skill catalog audit + suppression policy.** The 12 superpowers skills are split into "actively used" (`receiving-code-review`, `verification-before-completion`, `writing-plans`, `dispatching-parallel-agents`, `subagent-driven-development`, `using-git-worktrees`, `executing-plans`, `systematic-debugging`, `test-driven-development`, `requesting-code-review`, `finishing-a-development-branch`, `using-superpowers`, `brainstorming`, `writing-skills`) versus "noise / suppress" (none currently identified, but the manifest sets the policy). Add a `superpowers-suppression.md` doc identifying any plugin-shipped behavior that conflicts with Impossible OS doctrine (e.g. AI-attribution trailers, autonomous-agent assumptions). One row in CLAUDE.md "Mandatory Skill Triggers" table per actively-used superpowers skill, with the same hard-gate enforcement.
- **MCP usage discipline -- documented, not enforced.** A new `docs/infrastructure/mcp-usage.md` describes when to call which MCP tool: prefer `lsp-bridge` `definition` / `references` over `grep` for symbol queries; use `todo-graph` `ready` / `blocking` / `backlinks` before manually walking TODO XREFs; the `code` / `code-by` queries connect a source file to its owning TODO. CLAUDE.md "Using your tools" section gets a 3-bullet pointer at the doc. No hooks force this; the change is doctrine-level so future skill iterations adopt the pattern.
- **Cross-tool drift detection.** New `scripts/audit-ai-system.sh` runs three checks: (1) `.mcp.json` server set matches `~/.codex/config.toml` `[mcp_servers.*]` set; (2) every `Skill(codex-...)` invocation in `.claude/skills/` uses `${CLAUDE_PLUGIN_ROOT}/scripts/codex-companion.mjs` not the absolute marketplaces path; (3) every `codex-*` skill carries the receiving-code-review reminder paragraph. Sub-test added to `scripts/test-tooling.sh`.
- **Documentation sync** -- CLAUDE.md gets the Codex Invocation Policy paragraph and a 3-bullet "MCP usage" pointer; `docs/infrastructure/ai-system.md` Hook Routing Matrix gets the new hooks; `docs/infrastructure/mcp-usage.md` is new; the Skills table in CLAUDE.md absorbs the four superpowers skills as "Mandatory Skill Triggers" rows.

## Implementation Order

| ⭐  | Order | Section | Deliverable                                                                          | Depends On    | Status |
| --- | :---: | :-----: | ------------------------------------------------------------------------------------ | ------------- | :----: |
| ⭐  |   1   |  §1     | Wire `todo-graph` + `lsp-bridge` MCP into Codex CLI; cross-config drift validator    | --            |  [ ]   |
| ⭐  |   2   |  §2     | Codex invocation policy: no `--model` / `--effort` from Claude, hook block           | §1            |  [ ]   |
| ⭐  |   3   |  §3     | `receiving-code-review` hard gate: state file + post-Codex hook block                | §2            |  [ ]   |
| ⭐  |   4   |  §4     | Section-commit gate hardens to BLOCK with build + Codex + receiving evidence         | §3            |  [ ]   |
| ⭐  |   5   |  §5     | `review-todo-section` four-dispatch enforcement (state file + commit-gate check)     | §4            |  [ ]   |
| 💎  |   6   |  §6     | MCP usage discipline doc; CLAUDE.md pointer; skill audits update prose               | §1            |  [ ]   |
| 💎  |   7   |  §7     | Hook system audit + dedupe; `MANIFEST.md`; `scripts/audit-hooks.sh`                  | §3, §4, §5    |  [ ]   |
| 💎  |   8   |  §8     | Superpowers skill catalog audit; suppression policy; CLAUDE.md trigger rows          | §3            |  [ ]   |
| ⭐  |   9   |  §9     | Cross-tool drift detection: `scripts/audit-ai-system.sh` + test-tooling sub-test     | §1, §2, §7    |  [ ]   |
| 💎  |   10  |  §10    | Documentation sync: CLAUDE.md / ai-system.md / mcp-usage.md; reciprocal XREF sweep   | §1-§9         |  [ ]   |

> 💎 = parity work -- standard developer-tooling hygiene (audit scripts, drift detection, doc sync). Linux kernel ships `MAINTAINERS` + `get_maintainer.pl`; Windows has the engineering-systems-internal equivalent. We need it because skill / hook / MCP wiring drifts silently.
> ⭐ = competitive edge -- enforcing reviewer-contract discipline at the Bash / Edit tool boundary, hard-gating skill-pipeline steps, and cross-wiring two AI assistants (Claude Code + Codex CLI) into the same MCP server set is novel ground. Neither Win11 nor Linux ships AI-tooling automation at this layer.
> **Order vs section number:** §1 unblocks every other section (Codex needs MCP first; the model-flag policy + receiving-review gate + section-commit gate all assume Codex is reachable from Claude with shared context). §2 must precede §3 because the hook in §2 gates `codex-companion.mjs` invocations; the §3 hook fires on the OUTPUT of those invocations. §4 wraps §3 into a commit gate. §5 extends §4 with the four-dispatch counting. §6 + §7 + §8 are independent audits that can interleave once §1-§5 ship. §9 cross-wires the audits. §10 closes the doctrine documentation in lockstep.

---

## 1. Codex MCP Wiring + Cross-Config Drift Validator

Wire the two MCP servers Claude Code already uses (`todo-graph` + `lsp-bridge`) into Codex CLI's MCP client so Codex review / rescue / task runs have the same dependency-graph + LSP-grade code intelligence Claude has. Today every Codex review starts cold: it has the file paths in the prompt but no way to ask "what TODO owns this section?" or "where else is this symbol used?". Wiring the MCP servers into both sides closes that asymmetry. Codex CLI 0.125.0+ supports `codex mcp add` per `codex --help`; this section uses that surface.

- [ ] Add `[mcp_servers.todo-graph]` block to [`~/.codex/config.toml`](file:///home/derickpayne/.codex/config.toml): `command = "python3"`, `args = ["scripts/todo-graph/mcp_server.py"]`, `cwd = "/home/derickpayne/impossible-os"` (Codex CLI does not honor a per-project trust-block-scoped relative cwd, so the absolute path is required for portability across `codex` invocations from any directory).
- [ ] Add `[mcp_servers.lsp-bridge]` block to [`~/.codex/config.toml`](file:///home/derickpayne/.codex/config.toml): same shape, `args = ["scripts/lsp-mcp/bridge.py"]`. Confirm the bridge starts cleanly under Codex's MCP client (Codex uses the standard stdio MCP transport, same as Claude Code; the bridge already runs under FastMCP stdio per TODO-07 §1).
- [ ] Run `codex mcp list` and confirm both servers appear with status "ready" (or equivalent). Capture stdout in the verification.
- [ ] Run a smoke task: `codex exec "list 5 TODO sections that are ready to start using the todo-graph MCP"` and verify the output names actual ready sections (not hallucinated). Repeat for `lsp-bridge` with `codex exec "what is the prototype of pmm_alloc_contiguous? use the lsp-bridge definition tool"`.
- [ ] Add a startup-time drift validator to [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh): a new sub-test `t_pass mcp_drift "Claude .mcp.json and ~/.codex/config.toml expose the same MCP server set"` that parses both files and diffs the server name set. Fail fast on mismatch with a one-line actionable message.
- [ ] Document the wiring in [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) "MCP Server Set" subsection: one paragraph that names the two servers, the two clients (Claude Code via `.mcp.json`, Codex CLI via `~/.codex/config.toml`), the canonical source (this TODO §1), and the drift validator entry point.
- [ ] Commit: `"codex/mcp: wire todo-graph + lsp-bridge into Codex CLI config; add cross-config drift validator"`

**Test checkpoint:** `codex mcp list` shows `todo-graph` + `lsp-bridge`. `codex exec "..."` smoke prompts return real data from both servers. `bash scripts/test-tooling.sh` shows the new `mcp_drift` sub-test PASS. Manually deleting one of the four config blocks and re-running the validator triggers the sub-test FAIL with the expected actionable message. Test on: Linux WSL2 dev host (Codex CLI 0.125.0+).

## 2. Codex Invocation Policy: No `--model` / `--effort` From Claude

Document the user's policy and add a hook that blocks accidental violations. The user's stated rule: when Claude invokes ANY Codex command (`/codex:adversarial-review`, `/codex:review`, `/codex:rescue`, raw `codex-companion.mjs task`, `codex exec`), Claude does NOT pass `--model` or `--effort`; Codex CLI uses the configured default from `~/.codex/config.toml` (today: `model = "gpt-5.5"`, `model_reasoning_effort = "medium"`). The intent is that the user controls model + reasoning effort centrally without Claude second-guessing per dispatch.

> [!IMPORTANT]
> **Models in scope:** Impossible OS uses Claude Opus 4.7 in Claude Code and GPT 5.5 in Codex (per the user's standing instruction; this is not negotiable in this TODO). Existing skill prose under `.claude/skills/codex-*/` that hints at "use --effort xhigh for deep review" or names a specific model is reviewed and stripped or qualified.

- [ ] Add a "Codex Invocation Policy" paragraph to [`CLAUDE.md`](../../CLAUDE.md) under Model Roles: explicitly state "When invoking Codex from Claude (any of the 9 in-repo `codex-*` skills, the Codex plugin commands, or direct `codex-companion.mjs` calls), do NOT pass `--model` or `--effort`. Let Codex CLI use its configured defaults from `~/.codex/config.toml`. The user controls those values centrally."
- [ ] Audit the 9 in-repo codex-* skills under [`.claude/skills/`](../../.claude/skills/) (`codex-adversarial-review-section`, `codex-consistency-audit`, `codex-design-review`, `codex-fix-review`, `codex-impact-analysis`, `codex-perf-review`, `codex-review-todo`, `codex-test-coverage`, `codex-quality-review-section` if present): grep each `SKILL.md` for `--model`, `--effort`, `gpt-5`, `model_reasoning_effort`. Strip every example that hard-codes a value; replace with prose stating "use Codex CLI defaults".
- [ ] Audit [`.claude/skills/codex-adversarial-review-section/SKILL.md`](../../.claude/skills/codex-adversarial-review-section/SKILL.md) step 3 for the absolute-path `node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs"` invocation; replace with `node "${CLAUDE_PLUGIN_ROOT}/scripts/codex-companion.mjs"` or document why the absolute path is required (the Codex plugin's own `commands/adversarial-review.md` uses `${CLAUDE_PLUGIN_ROOT}` which is the canonical pattern).
- [ ] Add a new hook [`.claude/hooks/codex_model_flag_block.py`](../../.claude/hooks/codex_model_flag_block.py): PreToolUse on `Bash`, parses `tool_input.command`, blocks (exit 2) if the command contains `codex-companion.mjs` or starts with `codex ` or `codex exec` or `codex review` AND the same command line contains `--model` or `--effort` or `--model=` or `--effort=`. Opt-out: env var `CODEX_FLAG_OVERRIDE=1` (the agent must set it explicitly per call).
- [ ] Wire the new hook into [`.claude/settings.json`](../../.claude/settings.json) under the existing `Bash` matcher PreToolUse block. Order it after the existing inline gate hooks and before `accepted_xref_block.py`.
- [ ] Add a sub-test to [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh): `t_pass codex_flag_block "codex_model_flag_block.py exits 2 on --model and --effort"` -- pipes synthetic JSON tool_input into the hook script and asserts exit 2; pipes a clean command and asserts exit 0.
- [ ] Commit: `"codex/policy: document no --model/--effort policy; add hook to block accidental flag use"`

**Test checkpoint:** `bash scripts/test-tooling.sh` shows the `codex_flag_block` sub-test PASS. Manually triggering the hook with a synthetic `Bash(codex-companion.mjs adversarial-review --model gpt-5.5 ...)` invocation prints the block message and exits 2. Setting `CODEX_FLAG_OVERRIDE=1` allows the call through. Audit grep over `.claude/skills/codex-*/SKILL.md` shows no remaining `--model` or `--effort` example invocations. Test on: Linux WSL2 dev host.

## 3. `receiving-code-review` Hard Gate: State File + Post-Codex Hook Block

The user's `feedback_skill_invocation_drift.md` and `feedback_never_skip_review.md` both name the same failure mode: Claude runs Codex, gets a list of findings, and silently starts fixing without invoking `Skill(superpowers:receiving-code-review, ...)`. The reminder hook prints a `systemMessage` that the agent reads and acknowledges in prose, then proceeds anyway. Convert this from advisory to enforced.

- [ ] Add `.claude/state/` to `.gitignore`. Create the directory with a `.keep` file so the layout is documented.
- [ ] Define the state file shape at [`.claude/state/last-codex-review.json`](../../.claude/state/last-codex-review.json) (NEW, gitignored): `{"timestamp_ns": <int>, "trigger": "Skill(codex-adversarial-review-section)" | "Bash(codex-companion.mjs adversarial-review)" | ...,  "received": false, "received_timestamp_ns": <int|null>, "trigger_files": ["..."]}`. The shape doc lives in [`.claude/state/README.md`](../../.claude/state/README.md) (NEW).
- [ ] Add a PostToolUse hook [`.claude/hooks/codex_review_completed.py`](../../.claude/hooks/codex_review_completed.py): fires after `Skill` matcher when `tool_input.skill` is one of `codex-adversarial-review-section`, `codex-review-todo`, `codex-design-review`, `codex-impact-analysis`, `codex-test-coverage`, `codex-consistency-audit`, `codex-perf-review`, `codex-fix-review`; AND after `Bash` matcher when `tool_input.command` matches `codex-companion.mjs adversarial-review`, `scripts/copilot-review.sh`, `codex review`, or any `/codex:adversarial-review` invocation. Writes the state file with `received: false`.
- [ ] Add a PostToolUse hook on `Skill` matcher: if `tool_input.skill == "superpowers:receiving-code-review"`, mark the state file `received: true` with `received_timestamp_ns`. Same hook also accepts `Skill(superpowers:receiving-code-review, ...)` typed without the namespace prefix (the marketplaces plugin sometimes resolves the bare name).
- [ ] Add a PreToolUse hook [`.claude/hooks/receiving_review_required.py`](../../.claude/hooks/receiving_review_required.py): on `Edit | Write | MultiEdit` against any code path (`.c|.h|.asm|.S|.py|.sh|.mjs|.ts|.md` outside `todo/`), reads the state file. If `received: false` AND `timestamp_ns` is within the last 30 minutes, BLOCK (exit 2) with: "Codex review at `<timestamp_iso>` (trigger: `<trigger>`) has not been processed through `superpowers:receiving-code-review`. Invoke that skill to verify each finding at file:line and classify Fix / Reject / Accept BEFORE editing. Opt-out: set `RECEIVING_REVIEW_OVERRIDE=1` AND state in your next message what code-evidence quote justifies skipping (e.g. 'review found zero findings, output was \"no issues\"')."
- [ ] Wire both new hooks into [`.claude/settings.json`](../../.claude/settings.json). Order the PreToolUse `receiving_review_required.py` after `section_review_required.py` and before the existing test-side-effect ban.
- [ ] Add three sub-tests to [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh): (a) state file is written by the PostToolUse hook on a synthetic `Skill(codex-adversarial-review-section)` event; (b) the PreToolUse hook blocks (exit 2) with `received: false` state file; (c) the PreToolUse hook allows (exit 0) with `received: true` state file. Synthetic JSON inputs constructed with `printf` + Python.
- [ ] Update [`.claude/skills/codex-adversarial-review-section/SKILL.md`](../../.claude/skills/codex-adversarial-review-section/SKILL.md) step 4 to call out: "the new `receiving_review_required.py` hook will BLOCK any subsequent code edit until you invoke `Skill(superpowers:receiving-code-review, ...)`. The block is the gate; the existing `systemMessage` reminder is now the secondary signal."
- [ ] Commit: `"hooks: receiving-code-review hard gate via state file + Pre/PostToolUse pair"`

**Test checkpoint:** `bash scripts/test-tooling.sh` shows 3 new sub-tests PASS. End-to-end: invoke `/codex:adversarial-review` (or trigger `Skill(codex-adversarial-review-section)`) on a trivial change, attempt an Edit on a `.c` file, the hook blocks with the expected message; invoke `Skill(superpowers:receiving-code-review)`, retry the Edit, the hook allows. Setting `RECEIVING_REVIEW_OVERRIDE=1` allows the edit through. Test on: Linux WSL2 dev host.

## 4. Section-Commit Gate Hardens to BLOCK With Build + Codex + Receiving Evidence

Today's section-commit hook (the inline Python block in [`.claude/settings.json`](../../.claude/settings.json) under the `Bash` matcher PreToolUse for `git commit`) prints a long `systemMessage` listing implement-todo-section steps 13-18 and exits 0. The user's `feedback_never_skip_review.md` says this reminder mode does not work. Convert to a hard block when the staged diff carries the section-commit signature, with a deterministic check for build + Codex review + receiving-review evidence in the recent state.

- [ ] Define the section-commit signature: staged diff contains BOTH (a) at least one source-code path (`.c|.h|.asm|.S` under `src/` or `include/`, OR `.py|.mjs|.sh` under `scripts/`) AND (b) at least one TODO file (`todo/**/*.md`) where the unified diff shows an Implementation Order row's status column flipping from `[ ]` or `[/]` to `[x]` (regex over the diff hunks).
- [ ] Define the evidence requirements (all must be present):
   1. **Build evidence:** `tail -1 build/build.log` shows `=== BUILD OK ===` AND the file's mtime is within 30 minutes of `now`.
   2. **Codex evidence:** the state file at `.claude/state/last-codex-review.json` (from §3) shows a successful review with `received: true` AND `received_timestamp_ns` within 30 minutes, AND the `trigger_files` overlap with the staged source-code paths.
   3. **Receiving-review evidence:** baked into 2 (the §3 hook ensures `received: true` only when the `superpowers:receiving-code-review` skill ran).
- [ ] Replace the existing inline `git commit` PreToolUse hook in [`.claude/settings.json`](../../.claude/settings.json) with a script call to a new [`.claude/hooks/section_commit_gate.py`](../../.claude/hooks/section_commit_gate.py). The script: reads the staged diff via `git diff --cached`, detects the section-commit signature, and either exits 0 (no signature) or evaluates the 3 evidence checks and exits 2 with an actionable message naming the missing evidence(s). Opt-out: `SKIP_REVIEW_HOOK=1` AND the new `SKIP_REVIEW_HOOK_REASON="<text>"` env var (require both; the script logs the reason to `.claude/state/skip-log.jsonl` for paper trail).
- [ ] Update the inline `git commit` PostToolUse hooks (the smoke-test + boot-smoke ones) to read the `skip-log.jsonl` and prepend the most recent reason to their `systemMessage` if the commit was a `SKIP_REVIEW_HOOK=1` flow, so the user always sees in the next conversation turn what was skipped and why.
- [ ] Add a sub-test to [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh): synthetic staged diff with section-commit signature + missing build evidence -> exit 2; same diff with all 3 evidence pieces -> exit 0; same diff with `SKIP_REVIEW_HOOK=1` only (no reason) -> exit 2; same diff with both env vars -> exit 0 with logged reason.
- [ ] Update [`.claude/skills/implement-todo-section/SKILL.md`](../../.claude/skills/implement-todo-section/SKILL.md) step 18 to point at the gate: "the new `section_commit_gate.py` hook BLOCKS commits that lack build / Codex / receiving evidence. Steps 13-17 produce the evidence the gate checks; skipping any of them = blocked commit, not reminder-then-proceed."
- [ ] Update [`.claude/skills/verify-todo-section/SKILL.md`](../../.claude/skills/verify-todo-section/SKILL.md) (audit-mode wrapper) to note that the gate fires on the same staged-diff signature whether the source is implement-mode or verify-mode.
- [ ] Commit: `"hooks: section-commit gate is now a hard block, not a reminder"`

**Test checkpoint:** `bash scripts/test-tooling.sh` shows the new sub-tests PASS. End-to-end: stage a section-commit diff WITHOUT running Codex first, attempt `git commit`, hook exits 2 with the missing-evidence message; run the full implement-todo-section pipeline, retry, hook exits 0; export `SKIP_REVIEW_HOOK=1` without reason, exit 2 with usage message; export both env vars, exit 0 and the reason appears in `.claude/state/skip-log.jsonl`. Test on: Linux WSL2 dev host.

## 5. `review-todo-section` Four-Dispatch Enforcement

User feedback memory `feedback_codex_review_four_dispatches.md`: review-todo-section step 8 is FOUR separate Codex dispatches (adversarial / consistency / dead-code / perf), not one combined dispatch. The skill prose names the four; agents conflate them. Make the four-dispatch state machine-checkable.

- [ ] Define a state file [`.claude/state/last-review-stamps.json`](../../.claude/state/last-review-stamps.json) (gitignored): keyed by TODO file path, `{<todo_path>: {"section": "<§N>", "adversarial": <ts_ns|null>, "consistency": <ts_ns|null>, "dead_code": <ts_ns|null>, "perf": <ts_ns|null>}}`. Schema doc in [`.claude/state/README.md`](../../.claude/state/README.md).
- [ ] Update the §3 PostToolUse hook to also write to this file when the trigger is one of the four review-todo-section step-8 dispatches: `codex-adversarial-review-section` -> `adversarial`, `codex-consistency-audit` -> `consistency`, dead-code reviews come through `codex-perf-review` or `codex-impact-analysis` (the existing skill maps them; document explicitly), `codex-perf-review` -> `perf`. Mention the marker convention in the skill prose so future agents know which review counts as which dispatch.
- [ ] Update [`.claude/hooks/section_commit_gate.py`](../../.claude/hooks/section_commit_gate.py) (from §4): when the staged diff includes the section-commit signature AND the TODO file shows a section-level `**Verified:**` or `**Quality reviewed:**` stamp being added in the diff, ALSO require all four state-file entries for that TODO path are within the last 30 minutes. If any are missing, exit 2 with the actionable list of missing dispatches.
- [ ] Add a sub-test: synthetic state-file with 3 of 4 dispatches recorded -> exit 2 naming the missing one; all 4 -> exit 0.
- [ ] Update [`.claude/skills/review-todo-section/SKILL.md`](../../.claude/skills/review-todo-section/SKILL.md) step 8 to call out the state-file mechanism: "after dispatching each of the four reviews, the §3 PostToolUse hook records the dispatch. The §4 commit gate refuses the section-commit until all four are present. There is no 'three is enough' fallback."
- [ ] Commit: `"hooks: enforce review-todo-section step-8 four-dispatch policy via state file"`

**Test checkpoint:** End-to-end on a `verify-todo-section` re-stamp: run only adversarial + consistency, attempt to commit a `**Quality reviewed:**` stamp, hook exits 2 naming `dead_code` + `perf` missing; run the remaining two, retry, exit 0. `bash scripts/test-tooling.sh` shows the new sub-test PASS. Test on: Linux WSL2 dev host.

## 6. MCP Usage Discipline -- Doc + CLAUDE.md Pointer

Today neither Claude nor Codex consistently uses the two MCP servers. Symptom: Claude greps for symbols when a `lsp-bridge.definition` would be one call; Claude walks TODO XREFs by hand instead of calling `todo-graph.backlinks`. Document the doctrine; let the agent's planning skill (and future skill iterations) pick it up.

- [ ] Create [`docs/infrastructure/mcp-usage.md`](../../docs/infrastructure/mcp-usage.md) (NEW). Sections: (a) Inventory -- name the two servers and link to TODO-06 §8 + TODO-07 as the canonical owners; (b) When to call `todo-graph` -- the 11 read-only queries (`ready` / `blocked` / `blocking` / `by-domain` / `backlinks` / `deferred` / `deferred-by` / `orphans` / `stale` / `stats` / `code` / `code-by`) with one-line use case each; (c) When to call `lsp-bridge` -- the 6 read-only LSP tools (`hover` / `definition` / `references` / `diagnostics` / `workspace_symbol` / `document_symbol`) with one-line use case each, plus the discipline rule "prefer `definition` over `grep '^.*foo.*('`" and "prefer `references` over `grep -rn 'foo('`"; (d) When NOT to call MCP -- file-content reads stay on `Read`, file-content writes stay on `Edit`/`Write`, large-scale repo grep stays on `Bash(grep ...)`; (e) Cross-tool parity -- both Claude and Codex see the same MCP server set after §1 ships.
- [ ] Add a 3-bullet "MCP usage" pointer to [`CLAUDE.md`](../../CLAUDE.md) under "Using your tools": "(1) prefer `mcp__lsp-bridge__definition` / `references` over `grep` for symbol queries; (2) prefer `mcp__todo-graph__ready` / `backlinks` / `code` over manual TODO walks; (3) full doctrine at `docs/infrastructure/mcp-usage.md`."
- [ ] Audit the 11 task-execution skills under [`.claude/skills/`](../../.claude/skills/) (`implement-todo-section`, `review-todo-section`, `verify-todo-section`, `quality-review-section`, `complete-todo-file`, `validate-todo-file`, `gap-analysis-todo`, `create-todo`, `debug-session`, `diagnose-serial-log`, `implement-unit-tests`): grep for `grep -rn` and `Grep(` patterns; where the use case is "find symbol X" or "find references to function Y" or "find TODO mentions of Z", replace with the corresponding MCP call in skill prose. Keep the manual `grep` fallback for cases the MCPs do not cover (markdown body text search, scripts/, etc.).
- [ ] Update [`.claude/skills/create-todo/SKILL.md`](../../.claude/skills/create-todo/SKILL.md) step 3 (cross-TODO overlap scan) to use `mcp__todo-graph__backlinks <id>` for the dependency walk instead of pure Grep.
- [ ] Update [`.claude/skills/codex-impact-analysis/SKILL.md`](../../.claude/skills/codex-impact-analysis/SKILL.md) and [`.claude/skills/codex-consistency-audit/SKILL.md`](../../.claude/skills/codex-consistency-audit/SKILL.md) to call `mcp__lsp-bridge__references` first, then dispatch the Codex review with the reference-set context inline.
- [ ] Commit: `"docs/mcp: usage discipline doc; CLAUDE.md pointer; skill prose update"`

**Test checkpoint:** `docs/infrastructure/mcp-usage.md` exists, names the two servers + 17 total tools, validates as well-formed markdown via `bash scripts/lint.sh` (existing lint accepts new doc files). CLAUDE.md grep for "MCP usage" shows the new 3-bullet block. Skill grep for `grep -rn` in the 11 audited skills shows only fallback uses. Test on: Linux WSL2 dev host.

## 7. Hook System Audit + Dedupe

Today there are 14 Python files under [`.claude/hooks/`](../../.claude/hooks/) plus several inline-Python hooks in [`.claude/settings.json`](../../.claude/settings.json). Some overlap (`accepted_xref_block.py` blocks at commit, `accepted_xref_warn.py` warns at edit; `numeric_todo_shorthand.py` and `bare_section_refs.py` both touch TODO files; the inline domain-router and the dedicated quality-skill router fire on the same Edit events). No manifest documents which hook fires when, so adding a new hook risks shadowing or duplicating an existing one. After §3-§5 ship, the system gains 3-4 new hooks; document them all in one manifest and dedupe overlap.

- [ ] Create [`.claude/hooks/MANIFEST.md`](../../.claude/hooks/MANIFEST.md) (NEW). One row per hook: name, file path, matcher (Edit / Write / MultiEdit / Bash / Skill / global), event (PreToolUse / PostToolUse / SessionStart), 1-line purpose, exit codes, opt-out env var if any. Group by event then matcher. Total expected after §3-§5: ~20 hooks.
- [ ] Audit overlap: check whether `accepted_xref_block.py` (Bash PreToolUse on `git commit`) and `accepted_xref_warn.py` (Edit/Write/MultiEdit PostToolUse) cover non-overlapping events with consistent rules; merge or keep both with a manifest note.
- [ ] Audit the inline domain-router hook (the long inline `python3 -c` in `settings.json` PreToolUse Edit/Write) versus the per-file hooks; extract the inline router to a dedicated [`.claude/hooks/domain_quality_router.py`](../../.claude/hooks/domain_quality_router.py) so settings.json stays compact and the router can be unit-tested.
- [ ] Same extraction for the inline `git commit` smoke-test hook and boot-smoke hook into dedicated files [`.claude/hooks/post_commit_smoketest.py`](../../.claude/hooks/post_commit_smoketest.py) and [`.claude/hooks/post_commit_smoketest_boot.py`](../../.claude/hooks/post_commit_smoketest_boot.py). The `settings.json` entries become one-line `command: "python3 .claude/hooks/<name>.py"` calls.
- [ ] Same extraction for the two inline reminder hooks (the `Skill` matcher pipeline reminder; the section-commit reminder upgraded in §4 to a block).
- [ ] Add `scripts/audit-hooks.sh` (NEW): runs through `.claude/settings.json`, lists every matcher and its hook commands, cross-checks against `.claude/hooks/MANIFEST.md`, and reports any drift (hook in dir but not in manifest, manifest entry without a hook, settings.json command not pointing at a manifest entry). Sub-test added to `scripts/test-tooling.sh`.
- [ ] Commit: `"hooks: extract inline hooks; audit + manifest; dedupe overlap"`

**Test checkpoint:** [`.claude/hooks/MANIFEST.md`](../../.claude/hooks/MANIFEST.md) lists ~20 hooks with full metadata. `bash scripts/audit-hooks.sh` shows zero drift. `bash scripts/test-tooling.sh` shows the new audit sub-test PASS. `.claude/settings.json` size shrinks (inline-Python blocks replaced with one-line file calls). Test on: Linux WSL2 dev host.

## 8. Superpowers Skill Catalog Audit + Trigger Rows

The superpowers plugin ships 14 skills; CLAUDE.md "Mandatory Skill Triggers" today calls out only `superpowers:receiving-code-review`. Audit the other 13: which are actively useful in Impossible OS workflows, which conflict with our doctrine (e.g. anything that would add an AI-attribution trailer or assume autonomous-agent behavior), which have a hard mandatory trigger we should add, which are advisory.

- [ ] List every superpowers skill with a 1-line "use here" verdict: `brainstorming`, `dispatching-parallel-agents`, `executing-plans`, `finishing-a-development-branch`, `receiving-code-review` (already mandatory), `requesting-code-review`, `subagent-driven-development`, `systematic-debugging`, `test-driven-development`, `using-git-worktrees`, `using-superpowers`, `verification-before-completion`, `writing-plans`, `writing-skills`.
- [ ] For each "actively useful" skill, decide if it earns a row in CLAUDE.md "Mandatory Skill Triggers" with a hook-enforced gate. Strong candidates: `verification-before-completion` (any "task complete" claim should run it), `writing-plans` (TODO sections ARE plans -- already handled by `create-todo`), `requesting-code-review` (every Codex dispatch implicitly requests; document the link).
- [ ] For each "conflict with doctrine" item, document the conflict in [`docs/infrastructure/superpowers-policy.md`](../../docs/infrastructure/superpowers-policy.md) (NEW) -- e.g. "if any superpowers skill suggests adding `Co-Authored-By:` or `Assisted-by:` trailer, ignore; CLAUDE.md zero-trailer policy wins".
- [ ] Add 1-2 new rows to CLAUDE.md "Mandatory Skill Triggers" table for the strongest candidates from the audit. Wire enforcement via existing or new hooks where mechanical (e.g. a "task-complete claim without verification-before-completion" detection is non-trivial; document as "agent discipline" if not mechanizable).
- [ ] Update [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) skill-system section with a pointer at the new superpowers-policy doc.
- [ ] Commit: `"docs/superpowers: audit catalog; add mandatory trigger rows; suppression policy doc"`

**Test checkpoint:** [`docs/infrastructure/superpowers-policy.md`](../../docs/infrastructure/superpowers-policy.md) exists and names every superpowers skill with a verdict. CLAUDE.md "Mandatory Skill Triggers" table has at least 2 new rows. Test on: Linux WSL2 dev host (no runtime test; doctrine-level change).

## 9. Cross-Tool Drift Detection -- `scripts/audit-ai-system.sh`

Wire the per-section audit checks (§1 MCP drift, §2 Codex flag policy, §3 receiving-review hook coverage, §6 mcp-usage discipline, §7 hook manifest, §8 superpowers policy) into one umbrella script that catches drift between the Claude side, the Codex side, and the two installed plugins.

- [ ] Create `scripts/audit-ai-system.sh` (NEW). Six checks, each with `t_pass` / `t_fail` output and a 1-line actionable message on failure:
   1. `mcp_set_match` -- `.mcp.json` server-name set == `~/.codex/config.toml` `[mcp_servers.*]` keys (calls into §1 validator).
   2. `codex_skill_no_model_flag` -- grep over `.claude/skills/codex-*/SKILL.md` finds zero `--model ` and zero `--effort ` examples.
   3. `codex_skill_companion_path` -- every `codex-companion.mjs` invocation in skill prose uses `${CLAUDE_PLUGIN_ROOT}` (skill-side) or the absolute marketplaces path (documented exceptions only).
   4. `codex_skill_receiving_pointer` -- every `.claude/skills/codex-*/SKILL.md` mentions `superpowers:receiving-code-review` or `receiving-code-review` at least once.
   5. `hook_manifest_complete` -- every file under `.claude/hooks/` has a row in `MANIFEST.md`; every manifest row points at an existing file (calls into §7).
   6. `state_dir_layout` -- `.claude/state/` directory exists with the documented file shapes; `.gitignore` includes the directory.
- [ ] Wire `scripts/audit-ai-system.sh` into `bash scripts/test-tooling.sh` as a single sub-test (`t_pass audit_ai_system "ai-system drift checks pass"`).
- [ ] Add a CI-side equivalent: a job in [`.github/workflows/build.yml`](../../.github/workflows/build.yml) (if Github CI is wired -- TODO-01 §6 owns this) that runs `bash scripts/audit-ai-system.sh` before any other check. Failure on PR = same actionable message in the CI log.
- [ ] Commit: `"scripts: audit-ai-system.sh -- six cross-tool drift checks; wired into test-tooling"`

**Test checkpoint:** `bash scripts/audit-ai-system.sh` exits 0 with all 6 checks PASS. Synthetically delete the `lsp-bridge` block from `~/.codex/config.toml`, re-run, check 1 fails with the expected message. Restore. Synthetically add `--model gpt-5.5` to one skill's example, re-run, check 2 fails. Restore. `bash scripts/test-tooling.sh` shows the new umbrella sub-test PASS. Test on: Linux WSL2 dev host.

## 10. Documentation Sync -- CLAUDE.md / ai-system.md / mcp-usage.md / Reciprocal XREFs

Every section above touches a doctrine surface. Close the loop in lockstep with the implementation so the docs and the running code never diverge.

- [ ] CLAUDE.md changes (single commit at the end): add the "Codex Invocation Policy" paragraph (§2), the 3-bullet "MCP usage" pointer (§6), the new mandatory-trigger rows (§8), and a 1-line cross-reference to this TODO under "Mandatory Skill Triggers" overview text.
- [ ] [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md): update Hook Routing Matrix to add the new hooks from §3, §4, §5, §7; update External-Reviewer Contract subsection to point at the §3 hard gate; update MCP Server Set subsection to point at §1 cross-config.
- [ ] [`docs/infrastructure/mcp-usage.md`](../../docs/infrastructure/mcp-usage.md): finalized in §6.
- [ ] [`docs/infrastructure/superpowers-policy.md`](../../docs/infrastructure/superpowers-policy.md): finalized in §8.
- [ ] Reciprocal XREF sweep: add a `-> XREF: 00-infrastructure/TODO-08 §<N>` line to TODO-02 §3 (Hook Routing Matrix) referencing this TODO's §3-§5; to TODO-02 §4 (External-Reviewer Contract) referencing §3-§5; to TODO-06 §8 (todo-graph MCP) referencing §1, §6; to TODO-07 §1 (LSP-MCP bridge) referencing §1, §6.
- [ ] Update [`todo/00-infrastructure/INDEX.md`](INDEX.md) Active TODOs section: add a TODO-08 row with the same one-paragraph blurb style as TODO-06 / TODO-07.
- [ ] Update [`todo/TODO-00-INDEX.md`](../TODO-00-INDEX.md) (root index): same row addition.
- [ ] Update memory: record `feedback_codex_no_model_or_effort.md` (no --model / --effort policy), `feedback_mcp_usage_first.md` (prefer MCP over grep), `project_automation_hardening_complete.md` (TODO-08 done -- once shipped).
- [ ] Commit: `"docs: sync CLAUDE.md / ai-system.md / superpowers-policy + reciprocal XREFs for TODO-08"`

**Test checkpoint:** `bash scripts/audit-ai-system.sh` green; `bash scripts/test-tooling.sh` green; `bash scripts/build.sh` green (no kernel impact); `bash scripts/todo-graph/validate.py` green (no stale XREFs). Both indexes show the new TODO-08 row. Test on: Linux WSL2 dev host.

---

## Format Quick Reference

| Concern | Where it lives | Owner |
|---------|----------------|-------|
| Claude Code MCP servers | [`.mcp.json`](../../.mcp.json) | §1 (this TODO) -- mirrors to Codex |
| Codex CLI MCP servers | `~/.codex/config.toml` `[mcp_servers.*]` | §1 -- mirrors from Claude side |
| Codex model + effort default | `~/.codex/config.toml` top-level | User -- centralized control; §2 documents non-override policy |
| Hooks | [`.claude/settings.json`](../../.claude/settings.json) + [`.claude/hooks/`](../../.claude/hooks/) | §7 manifest; per-section adds |
| State files | `.claude/state/*.json` (gitignored) | §3, §5 -- shape doc in `.claude/state/README.md` |
| Reviewer contract | [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) "External-Reviewer Contract" | TODO-02 §4 owns doctrine; this TODO §3 owns mechanism |
| Skill triggers | [`CLAUDE.md`](../../CLAUDE.md) "Mandatory Skill Triggers" table | TODO-02 §3 owns doctrine; this TODO §8 adds rows |
| MCP usage | [`docs/infrastructure/mcp-usage.md`](../../docs/infrastructure/mcp-usage.md) | §6 (NEW) |
| Drift detection | `scripts/audit-ai-system.sh` | §9 (NEW) |

## OS Comparison

| ⭐ | Feature                                  | 🪟 Win11               | 🐧 Linux                | 🚀 Impossible OS                   |
|----|------------------------------------------|-----------------------|------------------------|-----------------------------------|
| 💎 | Cross-tool MCP server set                | ❌ N/A                 | ❌ N/A                  | ⬜ Planned -- §1                   |
| 💎 | Hook + skill manifest                    | ❌ N/A                 | ❌ N/A                  | ⬜ Planned -- §7                   |
| ⭐ | Reviewer-contract hard gate at edit      | ❌ Not available       | ❌ Not available        | ⬜ Planned -- §3                   |
| ⭐ | Section-commit gate with build evidence  | ❌ Not available       | ❌ Not available        | ⬜ Planned -- §4                   |
| ⭐ | Four-dispatch quality review enforcement | ❌ Not available       | ❌ Not available        | ⬜ Planned -- §5                   |
| ⭐ | Cross-tool drift detection (Claude+Codex)| ❌ Not available       | ❌ Not available        | ⬜ Planned -- §9                   |
| 💎 | Doctrine + skill catalog audit           | ❌ Internal eng only   | ⚠️ Ad-hoc per project   | ⬜ Planned -- §8                   |
| 💎 | MCP usage discipline doctrine            | ❌ N/A                 | ❌ N/A                  | ⬜ Planned -- §6                   |

> **After §1-§5:** Claude and Codex share the same MCP server set, the same Codex invocation policy, the same hard gates around reviews and section commits. Drift between the two automation sides is detectable.
> **After §6-§9:** The whole automation surface is auditable in one script. Hook system is documented; MCP usage is doctrine; superpowers plugin behavior is reconciled with Impossible OS rules.
> **After §10:** Doctrine docs match running code; reciprocal XREFs land. Future work in TODO-02 / TODO-06 / TODO-07 has a clean integration story.

## Unit Tests

> Tests for this TODO are tooling-side, not kernel-side. They live under `scripts/test-tooling.sh` (existing) plus the new `scripts/audit-ai-system.sh`. No `TEST_CAT_*` registration; no `src/kernel/test/test_*.c`.

- [ ] `scripts/test-tooling.sh` sub-tests (added across §1-§9):
  - `mcp_drift` -- §1 cross-config validator (Claude `.mcp.json` vs Codex `~/.codex/config.toml`)
  - `codex_flag_block` -- §2 hook exits 2 on `--model` / `--effort`
  - `receiving_state_write` -- §3 PostToolUse hook writes state file on Codex review trigger
  - `receiving_pre_block` -- §3 PreToolUse hook blocks Edit on `received: false`
  - `receiving_pre_allow` -- §3 PreToolUse hook allows Edit on `received: true`
  - `section_commit_gate_missing_build` -- §4 gate exits 2 with no build evidence
  - `section_commit_gate_missing_codex` -- §4 gate exits 2 with no Codex state file
  - `section_commit_gate_pass` -- §4 gate exits 0 with all evidence
  - `section_commit_gate_skip` -- §4 gate honors `SKIP_REVIEW_HOOK=1` + reason; logs to skip-log
  - `four_dispatch_partial` -- §5 gate exits 2 with 3 of 4 review state entries
  - `four_dispatch_full` -- §5 gate exits 0 with all 4
  - `audit_hooks_clean` -- §7 manifest matches dir + settings.json
  - `audit_ai_system` -- §9 umbrella runs all 6 checks
- [ ] No bat-runner subdirs needed (kernel test infra not touched).
- [ ] Commit: `"test: add automation-hardening tooling-side tests to test-tooling.sh"`

## Verification

- [ ] `codex mcp list` shows both `todo-graph` and `lsp-bridge` (§1)
- [ ] `codex exec "use todo-graph to list 5 ready sections"` returns real section titles (§1)
- [ ] `bash scripts/build.sh clean` -> `=== BUILD OK ===` (no kernel impact, just confirms tooling changes do not break build)
- [ ] `bash scripts/test-tooling.sh` shows all 13 new sub-tests PASS (§1-§5, §7, §9)
- [ ] `bash scripts/audit-ai-system.sh` shows all 6 checks PASS (§9)
- [ ] `bash scripts/todo-graph/validate.py` shows no stale XREFs after the §10 reciprocal-XREF sweep
- [ ] Manual end-to-end test: invoke `/codex:adversarial-review --wait` on a trivial branch change; attempt an Edit; receiving-review hook blocks; invoke `superpowers:receiving-code-review`; retry; allows (§3)
- [ ] Manual end-to-end test: stage a section-commit diff WITHOUT running Codex, attempt `git commit`, gate blocks with named missing evidence; complete the pipeline; retry; passes (§4)
- [ ] Manual end-to-end test: stage a `**Quality reviewed:**` stamp diff with only 2 of 4 review state entries, attempt commit, gate blocks naming the 2 missing dispatches (§5)
- [ ] Verify on: Linux WSL2 dev host (Codex CLI 0.125.0+, Claude Code current). Bare-metal / Windows test platforms not applicable -- this TODO is host-side automation only.
- [ ] Commit: `"00-infrastructure/TODO-08: automation hardening complete"`
