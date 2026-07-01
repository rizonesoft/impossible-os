# Hook Manifest -- `.claude/hooks/`

> Single source of truth for every hook script the Claude Code harness runs in this repo. Edit this file in lockstep with `.claude/settings.json`. `scripts/audit-hooks.sh` cross-checks the two and fails on drift.

## Conventions

- **Event:** `PreToolUse` (fires before the tool, can BLOCK with exit 2) / `PostToolUse` (after the tool, ignores exit code; outputs `systemMessage` JSON to nudge the agent) / `SessionStart` etc.
- **Matcher:** the tool-name pattern that gates the hook (`Edit|Write|MultiEdit`, `Bash`, `Skill`, `Bash|Skill`, `Edit|Write|MultiEdit|Skill|Bash`).
- **Kind:** `BLOCK` (sys.exit(2) on policy violation) / `REMINDER` (always exits 0; emits `systemMessage`) / `SIDE-EFFECT` (writes a file or runs a side-effect tool, never blocks) / `STATE` (reads/writes `.claude/state/*` to gate downstream hooks).
- **Wrap.sh:** `Y` = invoked through `sh .claude/hooks/wrap.sh <hook> <markers>` for fast-path skip on non-matching payloads. `N` = direct `python3 <hook>` (the marker prefilter would mis-classify; see `wrap.sh` header for the eligibility rules).
- **Opt-out:** environment variable that bypasses the gate when set to `1`. `--` = no opt-out (the policy is unconditional or owns its own opt-out logic internally).
- **Block-via:** for blocking hooks, how the block is signaled. Default = `sys.exit(2)` + stderr message. Two top-of-file allowlist markers: `# block-via: permissionDecision` (cleaner JSON `{"permissionDecision":"deny",...}` envelope; cleaner 2026 alternative; none today) and `# block-via: warning-only` (STATE hook that emits stderr warnings but never blocks; the audit recognizes this so STATE hooks can keep informative `stderr.write` paths without tripping the BLOCK exit-code check).

## PreToolUse Hooks

### Matcher: `Edit|Write|MultiEdit`

| Hook | File | Kind | Wrap.sh | Markers | Opt-out | Purpose |
|---|---|---|---|---|---|---|
| domain_quality_router | `.claude/hooks/domain_quality_router.py` | REMINDER | Y | `src/`, `include/kernel/`, `user/` | -- | Routes `.c/.h/.asm/.S` edits to the matching `*-code-quality` skill reminder (boot/desktop/shell/userland/kernel). Markers cover all routing prefixes: `src/` covers `src/boot/`, `src/desktop/`, `src/shell/`, `src/kernel/`, `src/apps/`; `user/` covers the standalone userland tree; `include/kernel/` covers kernel headers. |
| unicode_dash_block | `.claude/hooks/unicode_dash_block.py` | BLOCK | N | -- | -- | Rejects edits whose `new_string`/`content` contains U+2013 / U+2014. (Markers ARE the bytes -- prefilter would mis-route.) |
| numeric_todo_shorthand | `.claude/hooks/numeric_todo_shorthand.py` | BLOCK | N | -- | -- | Rejects N.M-style subnumbering (`### 17.1`, `**3.2 ...**`) in TODO files; one continuous checklist per `## N.` only. |
| scope_gap_marker | `.claude/hooks/scope_gap_marker.py` | BLOCK | N | -- | `SCOPE-GAP-ALLOWED:` per-line marker | Rejects `// TODO`, `// FIXME`, `// HACK`, `// for now`, `STATUS_NOT_IMPLEMENTED` placeholders in source; pushes scope-gap protocol. |
| test_side_effect_ban | `.claude/hooks/test_side_effect_ban.py` | BLOCK | N | -- | `TEST-SIDE-EFFECT-ALLOWED:` per-line marker | Rejects calls to `boot_progress`, `vpd_*`, `_init`, `panic`, `boot_halt` etc. in `src/kernel/test/test_*.c`. |
| bare_section_refs | `.claude/hooks/bare_section_refs.py` | BLOCK | N | -- | -- | Rejects bare `section`-sign + digit refs in `.c/.h/.asm` comments (must qualify with external spec). |
| citation_block | `.claude/hooks/citation_block.py` | BLOCK | N | -- | `SKIP_CITATION_BLOCK=1` | Mirrors `scripts/lint.sh` Check 13: rejects review/task citations in source comments (e.g. `(Codex H1 ...)`, `Codex M3 fix:`, `incident YYYY-MM-DD`, `see commit <hash>`, `per Codex review`). Vendored paths (`include/stb_truetype.h`, `src/libs/`) exempted. |
| notes_bloat_check | `.claude/hooks/notes_bloat_check.py` | BLOCK | N | -- | -- | Rejects TODO Notes blocks > 6 bullets, with sub-bullets / per-finding adoption sub-blocks, or any single bullet exceeding 250 chars. |
| todo_item_line_length | `.claude/hooks/todo_item_line_length.py` | BLOCK | N | -- | -- | Rejects new `- [ ]` / `- [x]` / `- [/]` checklist items in `todo/**/*.md` longer than 250 chars (whitelists `Commit:` items). Items are scannable summaries; file:line citations / commit hashes / fix-loop adoption traces belong in commit messages. |
| design_review_required | `.claude/hooks/design_review_required.py` | STATE+BLOCK | N | -- | `SKIP_DESIGN_REVIEW_HOOK=1` | After a flagship Skill is invoked, BLOCKs subsequent code edits until a Codex design dispatch is observed. Recognizes both the literal `design` keyword AND `[review-kind: adversarial\|consistency\|perf\|re-adversarial\|design]` markers as design-equivalent (TODO-08 §15 #1). Auto-clears the gate when a section-ship-style git commit (`review:` / `stamp:` / `docs:` / `todo:` subject prefix) lands AND the matching Bash tool_result returns successfully (TODO-08 §15 #2 + Codex H1 fix 2026-04-28; failed/in-flight commits do NOT clear). |
| receiving_review_required | `.claude/hooks/receiving_review_required.py` | STATE+BLOCK | N | -- | `RECEIVING_REVIEW_OVERRIDE=1` | After a Codex review completes, BLOCKs subsequent code edits until `receiving-code-review` discipline is applied. |
| section_review_required | `.claude/hooks/section_review_required.py` | STATE+BLOCK | N | -- | `SKIP_REVIEW_HOOK=1` (+ `SKIP_REVIEW_HOOK_REASON`) | After a section-ship commit, BLOCKs subsequent edits/Skills/non-script Bash until `/review-todo-section` is invoked. (Matcher is `Edit|Write|MultiEdit|Skill|Bash`.) |
| skill_step_block | `.claude/hooks/skill_step_block.py` | STATE+BLOCK+WARN | N | -- | `SKIP_SKILL_STEP_BLOCK=1` (+ `SKIP_SKILL_STEP_BLOCK_REASON` >= 12 chars) | On `git commit` and `Skill(review-todo-section)`, BLOCKs if the active multi-step skill's required terminal steps were never observed by `skill_step_observer`. On `Edit\|Write\|MultiEdit`, fires three TODO-08 partial-enforcement WARN-only heuristics (steps 1/2/3) when the first src/ Edit lacks a prior todo Read, has unread XREF targets, or follows fewer than 3 explore queries -- never blocks on the WARN path. (Matcher is `Bash\|Skill\|Edit\|Write\|MultiEdit`.) Reads `.claude/state/skill-progress.json` + `.claude/state/tool-history.jsonl`. TODO-08 §16: skips entries with `compaction_orphaned: true`; each skip is logged to `.claude/state/skill-progress-skip.log` (200-line JSONL ring buffer) for audit. |
| _heuristic_misses | `.claude/hooks/_heuristic_misses.py` | HELPER | N | -- | -- | Shared helper imported by `skill_step_block` + `section_commit_gate` for the TODO-08 partial-enforcement heuristics. `emit_warn(repo, step, signal, detail, todo_path, section)` writes one stderr WARN AND appends one structured JSON line to `.claude/state/heuristic-misses.jsonl` (5 MiB rotate to `.1`). All 7 WARN sites use this so the miss-log dataset stays consistent for the WARN -> ERROR promotion pipeline. |
| agent_dispatch_required | `.claude/hooks/agent_dispatch_required.py` | REMINDER | N | -- | -- | WS1b overnight-agent-discipline backstop. On `src/`/`include/kernel/` edits under an active overnight run (`OVERNIGHT_SEQUENCER_RUN`), WARNs (never blocks) when the section's exploration/audit appears to have skipped the default-on specialist agents (`kernel-explorer` / `kernel-quality-auditor` / `boot-quality-auditor`). Fail-open. |
| _skip_env | `.claude/hooks/_skip_env.py` | HELPER | N | -- | -- | TODO-08 §23 -- shared SKIP-env scanner used by all 7 PreToolUse gates that honor a `SKIP_*` (or similar) opt-out env. `read_skip_envs(cmd, keys, fallback_to_environ=True)` merges inline-cmd-env-prefix (`SKIP_FOO=1 git commit ...`, `env SKIP_FOO=1 ...`, `sudo SKIP_FOO=1 ...`) with `os.environ`; inline wins on key collision. Each consumer passes an EXPLICIT key list (not a prefix) to prevent suffix-collision capture of unrelated secrets. Migrated consumers: `section_review_required`, `section_commit_gate` (delegated `_scan_inline_env_prefix`), `phase1_evidence_gate`, `step5_quality_gate`, `skill_step_block`, `design_review_required`, `receiving_review_required`. |

### Matcher: `Bash`

| Hook | File | Kind | Wrap.sh | Markers | Opt-out | Purpose |
|---|---|---|---|---|---|---|
| accepted_xref_block | `.claude/hooks/accepted_xref_block.py` | BLOCK | N | -- | -- | At `git commit`, BLOCKs staged TODO diffs containing bare `> **Accepted:**` / `> **Deferred:**` XREFs lacking a parenthetical anchor. |
| codex_model_flag_block | `.claude/hooks/codex_model_flag_block.py` | BLOCK | N | -- | `CODEX_FLAG_OVERRIDE=1` | BLOCKs Bash invocations that pass `--model` / `--effort` / `-m` / `-e` / `-c model=...` to Codex. |
| section_commit_gate | `.claude/hooks/section_commit_gate.py` | BLOCK+WARN | N | -- | `SKIP_REVIEW_HOOK=1` (+ `SKIP_REVIEW_HOOK_REASON`); `SKIP_SMOKE_GATE=1` (+ `SKIP_SMOKE_GATE_REASON`) for §20 step-16 only | BLOCKs `git commit` of a section-ship signature without build + Codex + receiving-review evidence. TODO-08 §17 extension: also BLOCKs commits whose staged C/H diff matches step-13.5 triggers when no recent `re-adversarial` stamp exists. TODO-08 §20 extensions (impl-pipeline gates): step-8 test-wiring (require staged or HEAD `test_*.c` matching surface, or `**Note:** No <surface> test surface` exemption), step-13 impl-side adversarial dispatch (`[review-kind: adversarial-impl]` distinct from review-side), step-16 boot-path smoke test (validate `Boot complete` + `C:\>` markers in `build/smoke-test.stripped.log` with mtime > staged file mtime). TODO-08 partial-enforcement heuristics (4 WARN-only): step-9 test-coverage Codex dispatch missing on test-touching commits; step-15 no Edit between latest Codex and commit; step-17 thin validate phase (<2 Read/Grep since latest Codex); step-18 no TODO/FIXME/HACK/STATUS_NOT_IMPLEMENTED Grep since last src/ edit. WARN paths emit stderr + miss-log entry, never block. |
| phase1_evidence_gate | `.claude/hooks/phase1_evidence_gate.py` | BLOCK | N | -- | `SKIP_PHASE1_BLOCK=1` (+ `SKIP_PHASE1_BLOCK_REASON`) | TODO-08 §17: BLOCKs `Bash` dispatches of `codex-companion.mjs adversarial-review` with `[review-kind: adversarial]` when an active `review-todo-section` skill in `skill-progress.json` has fewer than 2 prior `Read`/`Grep` tool calls scoped to `src/` or `include/`. Walks `transcript_path` JSONL forward from `started_ts`; tool-history.jsonl carries no tool_input so transcript walk is the only viable evidence source. |
| step5_quality_gate | `.claude/hooks/step5_quality_gate.py` | BLOCK | N | -- | `SKIP_QUALITY_GATE_BLOCK=1` (+ `SKIP_QUALITY_GATE_BLOCK_REASON`) | TODO-08 §20 step-5: BLOCKs Edit/Write/MultiEdit on `src/` or `include/` paths when active `implement-todo-section` skill has not yet invoked the matching `<domain>-code-quality` skill (boot/kernel/desktop/shell/userland). Walks transcript JSONL forward from skill `started_ts`. Bootstrap-mode honored. |

### Matcher: `Skill`

| Hook | File | Kind | Wrap.sh | Markers | Opt-out | Purpose |
|---|---|---|---|---|---|---|
| skill_pipeline_reminder | `.claude/hooks/skill_pipeline_reminder.py` | REMINDER | Y | `implement-todo-section`, `implement-ssdt-range`, `review-todo-section`, `quality-review-section`, `create-todo`, `complete-todo-file` | -- | On flagship-skill invocation, emits the "no corner cutting / completion-first" reminder. |
| overnight_plugin_skill_block | `.claude/hooks/overnight_plugin_skill_block.py` | BLOCK | N | -- | `OVERNIGHT_PLUGIN_SKILL_OVERRIDE=1` | Rejects the unguarded plugin overnight skills (`overnight-runner:schedule`, `overnight-runner:start`) in this repo; overnight runs MUST arm via `.claude/skills/overnight-sequencer/arm-sequencer.sh` (run_phase_guard-protected, ChromeMCP-off, watchdog `*:0/10`). The plugin path arms no repo guard and thrashes on relaunch. |

## PostToolUse Hooks

### Matcher: `Bash`

| Hook | File | Kind | Wrap.sh | Markers | Opt-out | Purpose |
|---|---|---|---|---|---|---|
| post_commit_smoketest | `.claude/hooks/post_commit_smoketest.py` | REMINDER | N | -- (alias-aware) | -- | After a `git commit` (alias-aware) touching `.c/.h/.asm/.ld`, runs `scripts/test.sh QUIET=1` and reports via systemMessage. `if: Bash(git commit:*)`. |
| post_commit_smoketest_boot | `.claude/hooks/post_commit_smoketest_boot.py` | REMINDER | N | -- (alias-aware) | -- | After a `git commit` (alias-aware) touching boot-path files, runs `scripts/test-smoke.sh` and reports via systemMessage. `if: Bash(git commit:*)`. |
| codex_review_reception_reminder | `.claude/hooks/codex_review_reception_reminder.py` | REMINDER | Y | `adversarial-review` | -- | After a `codex-companion.mjs adversarial-review` Bash, emits the `receiving-code-review` discipline reminder. |

### Matcher: `Task`

| Hook | File | Kind | Wrap.sh | Markers | Opt-out | Purpose |
|---|---|---|---|---|---|---|
| agent_dispatch_recorder | `.claude/hooks/agent_dispatch_recorder.py` | STATE | N | -- | -- | After every `Agent`/Task dispatch, atomically records the dispatch (subagent type + HEAD sha + ts) into `.claude/state/last-agent-dispatch.json` so the WS1b `agent_dispatch_required` PreToolUse backstop can tell whether the default-on specialist agents ran for the current section. Never blocks; fail-open. |

### Matcher: `Bash|Skill`

| Hook | File | Kind | Wrap.sh | Markers | Opt-out | Purpose |
|---|---|---|---|---|---|---|
| codex_review_completed | `.claude/hooks/codex_review_completed.py` | STATE | N | -- | -- | Records the just-completed Codex dispatch into `.claude/state/last-codex-review.json` (gate state for `receiving_review_required` and the four-dispatch counter). Performs file-locking and hash gating. |
| skill_step_observer | `.claude/hooks/skill_step_observer.py` | STATE | N | -- | -- | After every Edit/Write/MultiEdit/Bash/Skill, walks transcript_path to find the active multi-step skill, looks up the step-evidence map, and records observed steps in `.claude/state/skill-progress.json`. Per-skill latest-invocation-wins; state keyed on `started_head_sha`. TODO-08 §16: skips entries with `compaction_orphaned: true` when picking the active skill, AND archives an existing-orphaned-entry-under-the-same-skill-name to `<skill>.orphan.<orphan_ts_ns>` before creating a fresh entry (prevents post-compaction step evidence from being recorded into an orphaned entry the blocker ignores). Imports `skill_step_map` for the evidence rules. |

### Matcher: `Edit|Write|MultiEdit`

| Hook | File | Kind | Wrap.sh | Markers | Opt-out | Purpose |
|---|---|---|---|---|---|---|
| todo_edit_reminder | `.claude/hooks/todo_edit_reminder.py` | REMINDER | Y | `todo/` | -- | After a TODO `.md` edit, suggests `validate-todo-file` if the change was structural. |
| todo_format_check | `.claude/hooks/todo_format_check.py` | REMINDER | N | -- | -- | After a TODO `.md` edit, reminds about Implementation Order / OS Comparison / stamp shapes. |
| test_wiring_reminder | `.claude/hooks/test_wiring_reminder.py` | REMINDER | Y | `src/kernel/test/` | -- | After a `test_*.c` edit, reminds the test-wiring checklist (registration, category, runner-init). |
| test_message_uniqueness | `.claude/hooks/test_message_uniqueness.py` | REMINDER | N | -- | -- | After a `test_*.c` edit, flags duplicate `TEST_ASSERT*` message strings. |
| test_pending_reminder | `.claude/hooks/test_pending_reminder.py` | REMINDER | N | -- | -- | After a `test_*.c` edit, flags `STATUS_NOT_IMPLEMENTED`-as-expected without `TEST_PENDING`. |
| skill_claudemd_sync_reminder | `.claude/hooks/skill_claudemd_sync_reminder.py` | REMINDER | Y | `.claude/skills/` | -- | After a skill `.md` edit, nudges to keep CLAUDE.md Skills table in sync if structural. |
| scope_gap_dedup_check | `.claude/hooks/scope_gap_dedup_check.py` | REMINDER | N | -- | -- | After a TODO edit, flags scope-gap dedup misses against existing TODOs. |
| accepted_xref_warn | `.claude/hooks/accepted_xref_warn.py` | REMINDER | N | -- | -- | After a TODO edit, warns on Accepted/Deferred XREFs lacking concrete parentheticals (commit-time blocker is `accepted_xref_block`). |
| todo_graph_auto_rewrite | `.claude/hooks/todo_graph_auto_rewrite.py` | SIDE-EFFECT | N | -- | -- | After a TODO `.md` edit, runs `todo-graph/validate.py --fix-line-numbers --write` to keep stamp parentheticals fresh. Surfaces ambiguity via systemMessage. |

## Helper Modules (imported by hooks, not invoked directly)

| Module | File | Importers | Purpose |
|---|---|---|---|
| skill_step_map | `.claude/hooks/skill_step_map.py` | `skill_step_observer.py`, `skill_step_block.py` | Step-evidence rules for the 5 multi-step skills + required-terminal-step lists. Pure data module; no `stderr.write` and no exit paths. |
| _postcommit_lock | `.claude/hooks/_postcommit_lock.py` | `post_commit_smoketest.py`, `post_commit_smoketest_boot.py` | Shared non-blocking flock helper at `/tmp/impossible-os-postcommit.lock` (overridable via `IMPOSSIBLE_OS_POSTCOMMIT_LOCK`). `try_acquire()` context manager + `emit_deferred()` JSON systemMessage helper + `run_with_group_timeout()` Popen wrapper that uses `start_new_session=True` + `os.killpg(SIGKILL)` on TimeoutExpired so QEMU/make descendants are reaped before the lock releases. (TODO-08 §31.) |
| runner_status | `.claude/hooks/runner_status.py` | `session_brief_inject.py`, `run_phase_guard.py` | Awareness-layer brief generator. Aggregates scattered run-state (guard cursor/phase, gate state-files, git, the doctrine Run Log) plus the curated `.claude/state/live-gotchas.md` registry into a compact brief. `full_brief(root)` is the SessionStart pull; `anchor_line(root)` is the one-line push the guard emits at phase transitions. Also a CLI (`runner_status.py [--root PATH] [--anchor]`). Read-only; fail-open; stdlib only. |
| test_postcommit_lock | `.claude/hooks/test_postcommit_lock.py` | (test harness, not a hook) | Standalone unit test for `_postcommit_lock.py` -- 6 sub-tests: first acquirer wins, second sees held, auto-release on context exit, deferred-message JSON shape, env override path, `run_with_group_timeout` reaps grandchild process group. Run: `python3 .claude/hooks/test_postcommit_lock.py`. Listed here for `audit-hooks.sh` Check 1 (every file in `.claude/hooks/` needs a manifest row); not invoked by the harness. |

## SessionStart / UserPromptSubmit / Stop / SubagentStop / PreCompact Hooks

| Hook | File | Event | Kind | Wrap.sh | Markers | Opt-out | Purpose |
|---|---|---|---|---|---|---|---|
| session_start | `.claude/hooks/session_start.py` | SessionStart | STATE | N | -- | -- | Writes `.claude/state/session.json` (sid + ts + cwd + head_sha); truncates `tool-history.jsonl` if > 7 days old; surfaces last `acknowledged-but-skipped.log` entry as systemMessage. |
| session_brief_inject | `.claude/hooks/session_brief_inject.py` | SessionStart | REMINDER | N | -- | -- | Awareness layer: injects `runner_status.full_brief(root)` (guard cursor/phase, gate state, git state, obligations, live-gotchas, recent decisions) as `additionalContext` so a fresh or post-compaction session re-orients without re-deriving run state. Read-only; fail-open. |
| user_prompt_doctrine | `.claude/hooks/user_prompt_doctrine.py` | UserPromptSubmit | REMINDER | N | -- | -- | Scans user prompt for bypass-shape phrases (`skip review`, `just commit`, `bypass the gate`, etc.) and injects systemMessage with the relevant doctrine + opt-out env vars. Never blocks. |
| stop_audit | `.claude/hooks/stop_audit.py` | Stop | STATE | N | -- | -- | At session-stop, walks transcript for "I will run X / I should invoke X" promises against enforcement-relevant skill names that never had a matching tool call within 5 turns; appends to `.claude/state/acknowledged-but-skipped.log`. Also flags TODO `[ ]`->`[x]` flips with no `superpowers:verification-before-completion` invocation. |
| subagent_audit | `.claude/hooks/subagent_audit.py` | SubagentStop | STATE | N | -- | -- | Records `{ts, subagent_type, duration_ms, tool_uses_total}` to `.claude/state/subagent-log.jsonl`. Flags runaway subagents (> 30 tool calls or > 10 min) into the same skip-log. |
| pre_compact_flush | `.claude/hooks/pre_compact_flush.py` | PreCompact | SIDE-EFFECT | N | -- | -- | TODO-08 §16: marks every active entry in `.claude/state/skill-progress.json` as `compaction_orphaned: true` (with `orphan_ts_ns` + `orphan_reason`) so post-compaction `skill_step_block` + `skill_step_observer` skip them, ending the catch-22 where a stale entry blocked every post-compaction commit. Idempotent. Then snapshots every `.claude/state/*.json` into `.claude/state/.compaction-snapshots/<ts>/` (retains 3 most recent). Also invalidates `.claude/state/transcript-scan-cache.json` (the design-review hook's offset cache; semantic window changes on compaction). |
| tool_history_writer | `.claude/hooks/tool_history_writer.py` | PostToolUse `*` | STATE | N | -- | -- | Appends `{event, tool_name, tool_use_id, ts_ns, duration_ms, success, target}` to `.claude/state/tool-history.jsonl` per tool call. `target` is the primary input identifier (file_path / pattern / first 240 chars of command), added by TODO-08 partial-enforcement heuristics so downstream gates can answer "was a todo/**/*.md Read before the first src/ Edit?" without re-scanning the transcript. Live-rotates at 10 MiB to `.1`; SessionStart truncates if older than 7 days. |
| overnight_guard | `.claude/hooks/overnight_guard.py` | SessionStart (`session-start`), UserPromptSubmit (`prompt`), Stop (`stop`) | BLOCK (Stop only) | N | -- | `overnight_guard.py pause/clear "<reason>"` | Overnight TODO-run guard for `/overnight-todo-runner` and the overnight-runner plugin. Tracks active run state in `.claude/state/overnight-run.json`; the Stop handler exits 2 (BLOCK) between section ships so the agent continues to the next `[ ]` section instead of final-answering; session-start/prompt handlers re-inject run context. Halt-on-error doctrine: `pause` with diagnosis on hard failure, never skip past. (Superseded by `run_phase_guard` for headless unattended runs; retained as interactive fallback pending retirement.) |
| run_phase_guard | `.claude/hooks/run_phase_guard.py` | PreToolUse (`pretool`, `Skill\|AskUserQuestion`), Stop (`stop`) | BLOCK | N | -- | `run_phase_guard.py clear "<reason>"` | Overnight-sequencer no-deviation phase machine. PreToolUse hard-blocks `AskUserQuestion` and any sequence skill called out of its phase; the armed marker (`.claude/state/sequencer-armed`) redirects the headless launch onto `overnight-sequencer` before the run is active. Stop exits 2 until FIXPOINT so an unattended run never voluntarily exits the queue. State: `.claude/state/sequencer-run.json` (run cursor only; deferrals live in the TODO files). CLI drives phase transitions; `selftest`. |
| sequencer_triage | `.claude/hooks/sequencer_triage.py` | (CLI helper, not a wired hook) | HELPER | N | -- | -- | Triage/traversal oracle over `build/todo-cache.json` + Verified-stamp scan: classifies each file/section DONE / DONE_UNSTAMPED / NEEDS_WORK and picks the next file in numeric traversal order (`--next` / `--classify` / `--summary` / `--selftest`). Skips non-impl doctrine files (INDEX.md, the runner-doctrine file). Used by the overnight-sequencer skill as the cursor source of truth. |

## Plugin Hooks (auto-loaded)

These hooks ship inside installed plugin caches under `~/.claude/plugins/cache/` and fire alongside our `.claude/settings.json` hooks. They are NOT edited by this repo; listed here so `audit-hooks.sh` can enumerate the full hook surface.

### `superpowers@claude-plugins-official` 5.0.7

`hooks/hooks.json`:

| Event | Matcher | Command | Purpose |
|---|---|---|---|
| SessionStart | `startup\|clear\|compact` | `${CLAUDE_PLUGIN_ROOT}/hooks/run-hook.cmd session-start` | Loads the superpowers session-start primer (skill catalog reminder, instruction-priority recap). |

### `codex@openai-codex` 1.0.2

`hooks/hooks.json`:

| Event | Matcher | Command | Purpose |
|---|---|---|---|
| SessionStart | -- | `node ${CLAUDE_PLUGIN_ROOT}/scripts/session-lifecycle-hook.mjs SessionStart` | Codex Companion session bootstrap (state reset). |
| SessionEnd | -- | `node ${CLAUDE_PLUGIN_ROOT}/scripts/session-lifecycle-hook.mjs SessionEnd` | Codex Companion teardown. |
| Stop | -- | `node ${CLAUDE_PLUGIN_ROOT}/scripts/stop-review-gate-hook.mjs` | Optional stop-time review gate (can be toggled via `/codex:setup`). 900s timeout. |

### `explanatory-output-style@claude-plugins-official` 1.0.0

`hooks/hooks.json`:

| Event | Matcher | Command | Purpose |
|---|---|---|---|
| SessionStart | -- | `bash ${CLAUDE_PLUGIN_ROOT}/hooks-handlers/session-start.sh` | Injects the explanatory-mode primer (the `Insight` block instructions) at session start. |

### `feature-dev@claude-plugins-official` (unknown version)

No `hooks/hooks.json` shipped under this plugin's installPath. Plugin contributes skills + commands + agents only. Listed here so audit-hooks.sh can recognize the absence as authorised, not drift.

### `chromemcp@rizonetech` 0.1.2

No `hooks/hooks.json` shipped. Plugin contributes the ChromeMCP browser-automation skills + MCP server wiring only. (Note: ChromeMCP is forbidden in overnight runs -- Impossible OS uses its own smoke-test infrastructure.)

### `overnight-runner@rizonetech` 0.1.3

No `hooks/hooks.json` shipped. Plugin contributes the overnight-runner skills (start/schedule/status) + launch scripts; the repo-side guard hook it relies on is `.claude/hooks/overnight_guard.py` (see the SessionStart/UserPromptSubmit/Stop table above), wired via `.claude/settings.json`, not via plugin hooks.

### `frontend-design@claude-plugins-official` (unknown version)

No `hooks/hooks.json` shipped. Plugin contributes the frontend-design skill only.

### `skill-creator@claude-plugins-official` (unknown version)

No `hooks/hooks.json` shipped. Plugin contributes the skill-creator skill only.

### `claude-md-management@claude-plugins-official` 1.0.0

No `hooks/hooks.json` shipped. Plugin contributes the revise-claude-md / claude-md-improver skills only.

### `desktop-commander@claude-plugins-official` (unknown version)

No `hooks/hooks.json` shipped. Plugin contributes the desktop-commander MCP server (filesystem + process tools) only. Listed here so audit-hooks.sh can recognize the absence as authorised, not drift.

### `code-modernization@claude-plugins-official` (unknown version)

No `hooks/hooks.json` shipped. Plugin contributes the legacy-analyst / business-rules-extractor / security-auditor / test-engineer / architecture-critic agents only. Listed here so audit-hooks.sh can recognize the absence as authorised, not drift.

### `claude-code-setup@claude-plugins-official` (unknown version)

No `hooks/hooks.json` shipped. Plugin contributes Claude Code setup/config helper skills + commands only. Listed here so audit-hooks.sh can recognize the absence as authorised, not drift.

### `commit-commands@claude-plugins-official` (unknown version)

No `hooks/hooks.json` shipped. Plugin contributes commit slash-commands only. Listed here so audit-hooks.sh can recognize the absence as authorised, not drift.

### `plugin-dev@claude-plugins-official` (unknown version)

No `hooks/hooks.json` shipped. Plugin contributes the agent-creator / plugin-validator / skill-reviewer agents + plugin-dev skills only. Listed here so audit-hooks.sh can recognize the absence as authorised, not drift.

### `remember@claude-plugins-official` 0.7.3

`hooks/hooks.json`:

| Event | Matcher | Command | Purpose |
|---|---|---|---|
| SessionStart | -- | `bash ${CLAUDE_PLUGIN_ROOT}/scripts/session-start-hook.sh` | Loads the `.remember/` persistent-memory primer (now/today/recent/archive/core buffers) at session start. |
| PostToolUse | -- | `bash ${CLAUDE_PLUGIN_ROOT}/scripts/post-tool-hook.sh` | Appends to the `.remember/` rolling history buffer after tool calls. |

> Plugin selection lives at `~/.claude/plugins/installed_plugins.json`. The retired `firecrawl@claude-plugins-official` plugin was uninstalled 2026-04-27. When installing or removing a plugin, add or remove the matching `### <name>@<marketplace>` subsection here in the same commit; `audit-hooks.sh` cross-checks the plugin index against the subsection headings.

## Wrap.sh eligibility quick reference

`wrap.sh <hook> <markers>` is a POSIX-shell prefilter that exits 0 immediately when none of the markers appear in the JSON payload. It saves ~25-35 ms of `python3` cold-start per non-matching tool call. Eligibility rules from `.claude/hooks/wrap.sh` header:

- Eligible (REMINDER hooks with cheap, accurate marker substrings; markers MUST cover every routing prefix the hook itself checks): `domain_quality_router` (`src/`, `include/kernel/`, `user/`), `skill_pipeline_reminder` (six gated skill names), `codex_review_reception_reminder` (`adversarial-review`), `todo_edit_reminder` (`todo/`), `test_wiring_reminder` (`src/kernel/test/`), `skill_claudemd_sync_reminder` (`.claude/skills/`).
- Ineligible: every BLOCK hook, every STATE hook, the alias-aware post-commit smoketest pair (the substring prefilter cannot replicate the `git -c alias.X='...' X` classifier in `section_commit_gate._bash_is_git_commit`), and `todo_graph_auto_rewrite` (the validator runtime dwarfs the prefilter saving).

When adding a new hook, default to direct `python3 <hook>` invocation; promote to `wrap.sh` only after confirming the hook is a pure REMINDER and that its triggering payloads contain a stable, distinctive substring.

## Exit-code policy (BLOCK hooks)

Every hook in this directory whose intent is to BLOCK on a policy violation MUST exit with code `2` (NOT `0`-with-stderr -- the latter silently passes; see [claude-code Issue #19561](https://github.com/anthropics/claude-code/issues/19561)). The cleaner 2026 alternative is to emit a `{"permissionDecision":"deny", ...}` JSON envelope; hooks taking that path declare it via a top-of-file `# block-via: permissionDecision` comment so `scripts/audit-hooks.sh` can allowlist the path. As of this manifest's writing, every BLOCK hook in this repo uses `sys.exit(2)`.

## Drift checks

`bash scripts/audit-hooks.sh` enumerates the full hook surface and reports drift in any direction:

1. Every file under `.claude/hooks/*.py` and `.claude/hooks/*.sh` has a row in this manifest.
2. Every manifest row points at an existing file.
3. Every command in `.claude/settings.json` references a path that resolves to a hook in this manifest (or is a documented inline command).
4. Every BLOCK hook's stderr-write path ends in `sys.exit(2)` (or carries the `# block-via: permissionDecision` opt-in marker).
5. Every plugin under `~/.claude/plugins/installed_plugins.json` has its `hooks/hooks.json` enumerated under "Plugin Hooks".

Run `bash scripts/audit-hooks.sh` after editing this manifest, after editing settings.json, or after installing/removing a plugin.
