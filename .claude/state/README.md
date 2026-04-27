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
  "trigger_blobs": {"scripts/foo.py": "abc123def456..."},
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
| `trigger_files` | list[str] | Source-code paths in the git index at trigger time (`git diff --cached --name-only` filtered to `.c`/`.h`/`.asm`/`.S` under `src`/`include` OR `.py`/`.mjs`/`.sh` under `scripts`). Populated by `codex_review_completed.py` for both Bash and Skill triggers. **Used by §4 commit gate** to bind a Codex review to the staged tree: gate requires every committed source path to be present in `trigger_files` (single state file; the latest Codex review supersedes earlier ones). Empty list = covers nothing (Codex C2: empty must NOT mean "covers everything"). May be empty if `git diff --cached` failed at trigger time -- treat as a missing review and re-run. |
| `trigger_blobs` | object[str → str] | `{path: git-blob-sha}` map captured by `git ls-files -s` at trigger time, one entry per `trigger_files` path. **Required by §4 commit gate** for content-binding (Codex C1): the gate compares the staged blob SHA at commit time against `trigger_blobs[path]`; a mismatch means the staged content was edited after the Codex review, so the review no longer covers the commit. Empty / missing → gate refuses ("review state missing trigger_blobs; re-run review"). |
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

### `last-review-stamps.json` -- review-todo-section four-dispatch policy

Owner: TODO-08-automation-hardening section 5 (`review-todo-section` Four-Dispatch Enforcement).

Tracks which of the review-todo-section step-8 dispatches have fired
for each TODO file path. The §4 section-commit gate refuses the commit
when a `**Verified:**` or `**Quality reviewed:**` stamp is being added
to a TODO file but the corresponding state entries are missing or
older than 30 minutes. Closes the documented failure mode
(`feedback_codex_review_four_dispatches`) where agents conflate the
separate dispatches into one shallow combined pass.

**Three-dispatch policy (post-2026-04-25):**

The user's `feedback_codex_review_four_dispatches` memory describes a
"FOUR dispatches" framing that predates the SKILL.md retirement of
the dead-code dispatch on 2026-04-25 (rationale: rubber-stamped
findings on functions reserved for future TODO sections introduced
regressions that built green and surfaced weeks later via SSDT / IDT
/ driver-vtable paths -- see `review-todo-section/SKILL.md` step 8).
The state file therefore tracks **three** dispatches:

| Key | Source skill | What it covers |
|---|---|---|
| `adversarial` | `codex-adversarial-review-section` | Step 5 adversarial pass: integer overflow, NULL deref, SMP races, ABI mismatch. |
| `consistency` | `codex-consistency-audit` | Step 8a: struct layout / ABI / SSDT row consistency / cross-file constants. |
| `perf` | `codex-perf-review` | Step 8b: hot-path allocation, ISR spinlock holds, O(n²) on unbounded inputs. |

**Schema:**

```json
{
  "todo/00-infrastructure/TODO-08-automation-hardening.md": {
    "section": "§5",
    "adversarial": 1735258800000000000,
    "adversarial_head": "7bbbe87b9d4...",
    "consistency": 1735258900000000000,
    "consistency_head": "7bbbe87b9d4...",
    "perf": 1735258950000000000,
    "perf_head": "7bbbe87b9d4..."
  }
}
```

| Field | Type | Purpose |
|---|---|---|
| key (TODO path) | str | Repo-relative path of the TODO file under review. Multiple sections of the same TODO share one entry; each new dispatch overwrites the prior timestamp for that kind. |
| `section` | str | Section number (`"§5"`, `"§11"`, etc.) extracted from the dispatch prompt. Recorded for audit; not used by the gate. |
| `adversarial` | int \| null | `time.time_ns()` of the most recent `codex-adversarial-review-section` Skill / Bash dispatch whose prompt named this TODO path. |
| `adversarial_head` | str (optional) | HEAD SHA at adversarial dispatch time. Codex M2 (post-impl): the §4 gate verifies this is an ancestor of (or equal to) the current HEAD before accepting the stamp; cross-branch stamp reuse is blocked at this layer. Legacy entries without the field pass on TTL alone with a one-shot stderr WARN during the transition window. |
| `consistency` | int \| null | Same as `adversarial` for `codex-consistency-audit`. |
| `consistency_head` | str (optional) | Same as `adversarial_head` for the consistency dispatch. |
| `perf` | int \| null | Same as `adversarial` for `codex-perf-review`. |
| `perf_head` | str (optional) | Same as `adversarial_head` for the perf dispatch. |

**Concurrency:** `_record_stamp` serializes the read-merge-write
sequence with `fcntl.flock()` on a sibling file `last-review-stamps.lock`
(also gitignored). Codex M3 (post-impl): without the lock, parallel
PostToolUse fires (e.g. `task --background` dispatches per
`feedback_codex_background_fallback`) raced on the same JSON --
both processes read the prior state, each merged only their own
kind, last writer won, and one of the three required dispatches was
silently dropped. The lockfile is idempotent and harmless if held
briefly across hooks; non-Linux hosts (no `fcntl`) fall back to the
prior unsafe path with the original race window.

**Dispatch attribution (how the hook knows which kind / TODO):**

PostToolUse `codex_review_completed.py` parses two fields per dispatch:

1. **Review kind** -- skill name when the trigger is a `Skill` tool call
   (one of `codex-adversarial-review-section` / `codex-consistency-audit`
   / `codex-perf-review`); OR a `[review-kind: adversarial|consistency|perf]`
   marker at the start of the prompt argument when the trigger is a
   `Bash(codex-companion.mjs adversarial-review "...")` call. The
   marker is mandatory in the codex-prompt-template.md so the bash
   path can disambiguate (the bash invocation is identical across
   all three dispatches -- only the prompt content differs).
2. **TODO path** -- regex `todo/[\w/-]+/TODO-\d+[-\w]*\.md` over the
   prompt argument. First match wins; multiple matches surface a
   stderr WARN ("ambiguous TODO target; using first").

If either field is missing, the hook records the trigger to
`last-codex-review.json` (§3 path) but skips writing
`last-review-stamps.json` and emits a stderr WARN. The gate then
treats the dispatch as not-counted for the four-dispatch check.

**Lifecycle:**

1. Agent dispatches (e.g.) `codex-consistency-audit` with a prompt
   containing `[review-kind: consistency]` and `todo/.../TODO-08-...md §5`.
2. PostToolUse hook writes `last-review-stamps.json["todo/.../TODO-08-...md"]["consistency"] = <ts_ns>`.
3. Repeat for the other two dispatches (adversarial, perf).
4. Agent stages a `**Verified:**` stamp diff for that TODO.
5. PreToolUse `section_commit_gate.py` detects the stamp-add, looks
   up the TODO entry, and confirms all three keys are non-null
   AND within the last 30 minutes. Missing or stale -> exit 2 with
   the actionable list of missing dispatches.

**Stale-pass invalidation:**

- 30-minute TTL: a dispatch older than 30 minutes does NOT count
  toward gate satisfaction. Agents must re-run the dispatch.
- Each new dispatch of the same kind for the same TODO overwrites
  the prior timestamp -- only the most recent one matters.
- **HEAD ancestry (Codex M2 post-impl):** when a dispatch entry
  carries a `<kind>_head` SHA, the gate verifies it's an ancestor
  of (or equal to) the current HEAD via `git merge-base --is-ancestor`.
  This handles the legitimate workflow where review fixes are
  committed between dispatches and the stamp commit (the fix commit
  is a descendant of the dispatch HEAD; ancestry holds), while
  rejecting cross-branch reuse (branch A's stamps cannot satisfy
  branch B's stamp commit unless A's HEAD is in B's history).
  Tree-hash binding is intentionally NOT enforced -- it would block
  the normal review-fix-stamp flow because tree hash changes between
  the dispatch and the stamp commit.

**Stamp-only commit path (Codex M1 post-impl):**

The §4 section-commit gate originally required source change AND
Implementation Order row flip to fire ("section" signature). The
user's actual workflow has TWO commits per section: the
implementation commit (source + row flip + sometimes stamp) and a
separate `stamp:` commit that adds the `**Verified:**` /
`**Quality reviewed:**` lines after the post-impl review pipeline.
The latter has no source change and no row flip -- under the
original signature it bypassed the four-dispatch check entirely.

The fix introduces a third signature class `stamp_only`: any staged
TODO file with an ADDED `> **Verified:**` or `> **Quality reviewed:**`
line (regex `^\+\s{0,3}>\s*\*\*(Verified|Quality reviewed):\*\*` --
tolerates 0-3 spaces of valid Markdown blockquote indent) AND no
staged source files. The `stamp_only` path runs ONLY the
four-dispatch check (no build / receiving / review_evidence checks,
since no source was reviewed). The standard `SKIP_REVIEW_HOOK=1` +
`SKIP_REVIEW_HOOK_REASON="<text>"` opt-out path applies; the audit
record carries `kind: "stamp_only"` so sweeps can distinguish.

**No-self-summary detector:**

The same hook scans the Codex prompt for first-person preambles
(`^\s*I (built|wrote|implemented|made)`, `^\s*Summary of changes`,
`^\s*What I built`, `^\s*My (implementation|approach)`) and emits a
stderr WARN naming the matched line. This is **not** a block --
the goal is to nudge agents toward the unbiased
`codex-prompt-template.md` shape per the
[CONSENSAGENT ACL-2025 sycophancy result](https://aclanthology.org/2025.findings-acl.1141/)
(implementor-narrative preambles bias the reviewer toward agreement).

### `skip-log.jsonl` -- section-commit gate opt-out audit trail

Owner: TODO-08-automation-hardening section 4 (section-commit gate hard block).

Append-only JSONL written by `.claude/hooks/section_commit_gate.py` whenever a
section commit takes the SKIP path (`SKIP_REVIEW_HOOK=1` + valid
`SKIP_REVIEW_HOOK_REASON`). Each line is a single JSON object; writes use
`os.open(O_APPEND|O_CREAT|O_WRONLY)` + a single `os.write()` under PIPE_BUF
(4096 bytes) so parallel sessions don't interleave (POSIX append-write
atomicity guarantee).

**Schema (one line per record):**

```json
{
  "timestamp_ns": 1735258800000000000,
  "mode": "harness" | "git-hook",
  "head_sha": "1408d994...",
  "staged_sources": ["src/foo.c", "scripts/bar.sh"],
  "flipped_todos": ["todo/00-infrastructure/TODO-08-automation-hardening.md"],
  "small_section_warn": false,
  "reason": "intentional revert-only commit, no source changes touched"
}
```

**Two record kinds (distinguished by presence of `kind`):**

- `kind` absent or `"SKIP"` -- a real opt-out commit. `reason` is required, >= 12 chars.
- `kind == "WARN"` AND `tag == "small-section-skip-risk"` -- a section commit
  with `< 50 LOC` source delta. Logged regardless of pass/fail outcome so a
  sweep over the log surfaces "small section, skipped review" patterns.

**SKIP is a state transition, not just an audit append (Codex M1):**

After a successful SKIP, `last-codex-review.json` is rewritten with
`received: false` AND `skipped_section_commit: true`. The next commit gate
evaluation cannot reuse the prior `received: true` to bypass evidence
checking. Without this reset, an agent could SKIP one section commit and
the leftover `received: true` would silently satisfy the next section's
gate.

**Post-commit propagation:** the smoke-test and boot-smoke PostToolUse hooks
in `.claude/settings.json` look up the most recent skip-log record matching
the current HEAD sha and prepend `[SKIP_REVIEW_HOOK reason: ...]` to their
systemMessage. Match by HEAD (not "last line globally") because parallel
sessions interleave (Codex M2).

## Gitignore

The directory ships tracked via `.keep`; per-session JSON files are excluded. See `.gitignore` rules at repo root:

```
.claude/state/*
!.claude/state/.keep
!.claude/state/README.md
```
