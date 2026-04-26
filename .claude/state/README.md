# `.claude/state/` -- Hook Runtime State

Per-session runtime state files used by Claude Code hooks. Gitignored
except for `.keep` (preserves the directory) and this README.

## Files

### `last-codex-review.json` -- receiving-code-review hard gate

Owner: TODO-08-automation-hardening section 3 (`receiving-code-review` Hard Gate).

Tracks whether the most recent external Codex review has been
processed through `superpowers:receiving-code-review` before any
subsequent Edit / Write / MultiEdit lands. Defends against the
documented failure mode (`feedback_skill_invocation_drift` +
`feedback_never_skip_review`) where Claude reads Codex findings,
acknowledges in prose, and silently starts editing without invoking
the receive skill.

**Schema:**

```json
{
  "timestamp_ns": 1735258800000000000,
  "trigger": "Bash(codex-companion.mjs adversarial-review)",
  "trigger_files": ["scripts/foo.py"],
  "head_sha": "1408d994...",
  "tree_hash": "sha256-of-git-status-porcelain",
  "received": false,
  "received_timestamp_ns": null
}
```

| Field | Type | Purpose |
|---|---|---|
| `timestamp_ns` | int | `time.time_ns()` at trigger fire. Anchors the 1-hour TTL invalidation. |
| `trigger` | str | What fired the trigger: `"Bash(<command-prefix>)"` or `"Skill(<skill-name>)"`. Surfaced in BLOCK envelope so the agent knows which review to receive. |
| `trigger_files` | list[str] | File paths the agent was working with when the trigger fired (best-effort, used only for the BLOCK message). May be empty. |
| `head_sha` | str | `git rev-parse HEAD` at trigger time. Recorded for audit / debugging; NOT used for invalidation per the design review (HEAD mismatch would let post-review file mutations clear the block, defeating the gate). |
| `tree_hash` | str | sha256 over `git status --porcelain`. Same rationale: recorded but NOT used for invalidation. |
| `received` | bool | False after a Codex trigger; flipped True when `Skill(superpowers:receiving-code-review)` (or the bare-name variant `receiving-code-review`) PostToolUse fires. |
| `received_timestamp_ns` | int \| null | `time.time_ns()` at the receive event. Used in audit; the gate's allow decision is based on `received`. |

**Lifecycle:**

1. Agent dispatches a Codex review (`Bash(node ...codex-companion.mjs adversarial-review ...)`, `Skill(codex-adversarial-review-section)`, etc.).
2. PostToolUse hook `codex_review_completed.py` writes the state file with `received: false` plus the `trigger` plus the snapshot of `head_sha` / `tree_hash`.
3. Agent invokes `Skill(superpowers:receiving-code-review)` to triage the findings.
4. PostToolUse hook on the receive Skill flips `received: true` plus sets `received_timestamp_ns`.
5. Subsequent `Edit` / `Write` / `MultiEdit` against any code file is allowed.

**If the agent skips step 3:**

The next `Edit` / `Write` / `MultiEdit` triggers PreToolUse hook `receiving_review_required.py` which reads the state file. With `received: false` AND `timestamp_ns` within the last 60 minutes, the hook exits 2 with the documented BLOCK envelope naming the trigger and the receive opt-out (`RECEIVING_REVIEW_OVERRIDE=1`).

**Stale-pass invalidation:**

- **TTL (1 hour)**: `now - timestamp_ns > 3600s` triggers expiry (allow). The agent has presumably moved on; a stale `received: false` from yesterday should not block today's edits.
- **HEAD / tree changes**: deliberately do NOT invalidate while `received: false`. If the agent edits between trigger and receive, the block stays until receive fires. (Codex design review High caught the original "stale-tree allow" semantics as a bypass.)
- **New review trigger**: each new Codex trigger overwrites the state with a fresh `received: false` plus new timestamp. Multiple reviews chain; only the latest matters for the gate.

**Why `tool_use_id` is not in the schema:**

The Claude Code harness PostToolUse / PreToolUse hooks receive `tool_name` plus `tool_input` via stdin JSON, but `tool_use_id` is not in that payload (verified live via probe). Binding the receive to the trigger's `tool_use_id` would require a harness-side change to expose the field. Until that lands, the trigger / receive correlation relies on the timestamp plus `received` boolean only. Documented for future-fix-when-feasible.

**Why no structured-finding parser:**

The upstream `superpowers:receiving-code-review` skill is process-prose ("READ then VERIFY then EVALUATE"), not structured JSON output (verified by reading the skill source at `~/.claude/plugins/cache/claude-plugins-official/superpowers/<ver>/skills/receiving-code-review/SKILL.md`). Parsing structured findings from its output would require either modifying the upstream skill (out of scope) or establishing a marker-file convention (no current pattern). Deferred to a future TODO-08 section.

## Gitignore

The directory ships tracked via `.keep`; per-session JSON files are excluded. See `.gitignore` rules at repo root:

```
.claude/state/*
!.claude/state/.keep
!.claude/state/README.md
```
