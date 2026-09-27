<!-- docs: covers=todo/00-infrastructure/TODO-08-automation-hardening.md -->
# Superpowers Plugin Policy

> Authority: this file. Pointed at from [CLAUDE.md "Plugin skills -- usage notes"](../../CLAUDE.md#plugin-skills----usage-notes) and [docs/infrastructure/ai-system.md](ai-system.md). Implements the skill catalog audit + trigger rows from [TODO-08 Automation Hardening](../../todo/00-infrastructure/TODO-08-automation-hardening.md).

The `superpowers@claude-plugins-official` plugin (5.0.7) ships 14 skills under `~/.claude/plugins/cache/claude-plugins-official/superpowers/5.0.7/skills/`. Some are load-bearing in this repo, some are advisory, and some carry assumptions that conflict with Impossible OS doctrine. This document is the per-skill verdict + the suppression rules that override the plugin defaults.

## Per-Skill Verdict

| Skill | Verdict | Rationale |
|---|---|---|
| `using-superpowers` | **MANDATORY (auto-loads at session start)** | Load-bearing primer; teaches the skill-invocation discipline the rest of the system depends on. Never disable. |
| `receiving-code-review` | **MANDATORY (hook-enforced)** | Wired into all 9 in-repo `codex-*` skills + the `receiving_review_required.py` PreToolUse hook. Verifies every Codex finding at file:line with Fix/Reject/Accept rigor before any code edit. Already in CLAUDE.md "Mandatory Skill Triggers." |
| `verification-before-completion` | **MANDATORY (skill-wired)** | Wired into `review-todo-section` step 15.5 and `complete-todo-file` Execution Discipline. Closes the "claims without re-running build/tests/lint" failure mode (`feedback_never_skip_review`). New CLAUDE.md row added by this audit. |
| `systematic-debugging` | **MANDATORY (when triggered)** | Bug / test-failure / unexpected-behavior signal must trigger this skill BEFORE proposing fixes. Repo's `debug-session` skill cross-references it. New CLAUDE.md row added by this audit. |
| `dispatching-parallel-agents` | **Useful (advisory)** | Referenced from `review-todo-section` step 8 to dispatch the consistency + perf Codex reviews in parallel. Use whenever 2+ tool calls are independent and parallelizable. No hook gate; agent discipline. |
| `requesting-code-review` | **Useful (implicit)** | Every Codex dispatch (`codex-companion.mjs adversarial-review` from any of the 9 in-repo `codex-*` skills) IS a code-review request. The skill's prose is informational; the 9 in-repo skills are the canonical request mechanism. No new trigger needed -- the codex-* skills already cover it. |
| `writing-plans` | **Useful (covered by `create-todo`)** | Impossible OS uses TODO files as the plan format; the in-repo `/create-todo` skill is the canonical entry point and covers this superpowers skill's intent. Use `writing-plans` only for sub-TODO multi-step planning that doesn't merit its own TODO file. |
| `executing-plans` | **Advisory** | Workflow guidance for separate-session execution with review checkpoints. Impossible OS's `/implement-todo-section` is the in-repo equivalent and is more rigorous (Codex four-dispatch + section-commit gate). Use only for ad-hoc multi-step task execution outside the TODO pipeline. |
| `subagent-driven-development` | **APPROVED for `user/`, `src/apps/`, tooling; FORBIDDEN for `src/kernel/`, `src/boot/`, `include/kernel/`** | See "Scope distinction" below. The plugin's canonical "dispatch -> subagent -> code-review subagent -> next" loop is fine for greenfield TDD or routine refactoring but is **not a substitute** for cross-MODEL adversarial review on security-sensitive kernel/boot code. |
| `test-driven-development` | **Useful (per-section discipline)** | Repo's `implement-unit-tests` skill is the canonical wiring; TDD is the preferred authoring style for new test surfaces. The skill's strict red/green/refactor cycle is advisory, not mandatory -- the kernel test framework can't always run a single test in isolation pre-build. |
| `using-git-worktrees` | **Advisory (rarely needed here)** | Single-developer repo with linear `main` history; isolation via worktree is cheap but not required. Use only when running a long-lived experiment (e.g., kernel ABI rewrite) in parallel with normal work. |
| `finishing-a-development-branch` | **Advisory** | Useful when a feature branch needs merge/PR/cleanup decisioning. Impossible OS uses linear `main` (no PR workflow today), so the merge/PR options are usually no-ops. The cleanup checklist is occasionally useful. |
| `brainstorming` | **Useful (advisory)** | Pre-implementation creative work. Pairs naturally with `/create-todo` or `/gap-audit-todo`. No hook gate; agent discipline ("requirements before code"). |
| `writing-skills` | **MANDATORY (when authoring or editing skills)** | Repo policy: every new or edited `.claude/skills/*/SKILL.md` walks this skill's authoring rubric. Repo also defines `docs/infrastructure/skill-authoring.md` with Impossible-OS-specific lifecycle rules (pair with `writing-skills`, do not replace). |

## Doctrine Conflicts and Suppression Rules

Where a superpowers skill suggests an action that conflicts with Impossible OS doctrine, the doctrine wins. The conflicts found by audit:

- **AI-attribution trailers.** If any superpowers skill suggests adding `Co-Authored-By:`, `Assisted-by:`, or any other AI-attribution trailer to a commit message, IGNORE. CLAUDE.md "Commits -- zero AI-attribution trailers" wins. The trailer itself is the bug; AI orchestration is implicit in the project's identity.
- **Autonomous-agent assumptions.** Any superpowers skill that assumes a sandboxed-PR-opening cloud agent (Devin/Cognition shape) is misaligned with Impossible OS doctrine -- this repo only accepts interactive Claude Code commits per CLAUDE.md "Autonomous-agent boundary." Read these skills as describing *patterns*, not licensed *automation*. The agent-discipline phrasing is fine; the autonomous-PR phrasing is not.
- **Mock-the-database test patterns.** `test-driven-development` references mock-heavy unit-test patterns. Impossible OS prefers concrete integration paths where possible (memory of incidents 2026-Q1 where mocked tests masked migration breakage). Apply TDD's red/green/refactor *cycle*, not necessarily its *isolation maximalism*.
- **GitHub-PR-thread-reply mechanics in `receiving-code-review`.** Impossible OS uses local Codex review (no PR thread). Treat that section as informational; the rest of `receiving-code-review` is fully load-bearing.

## Scope Distinction: subagent-driven-development vs Codex Four-Dispatch

[Jesse Vincent's superpowers writeup (Oct 2025)](https://blog.fsck.com/2025/10/09/superpowers/) describes a "dispatch -> subagent -> code-review subagent -> next" loop as the canonical pattern. This pattern is approved for some Impossible OS surfaces and forbidden for others.

| Surface | Subagent-driven-development | Codex four-dispatch |
|---|---|---|
| `src/kernel/`, `src/boot/`, `include/kernel/` | **Forbidden** as a substitute | **Mandatory** (adversarial + consistency + perf, +re-adversarial when the step-13.5 trigger fires) |
| `user/`, `src/apps/`, tooling, docs | **Approved** | Optional (only when the work touches a security-sensitive boundary) |
| `.claude/`, `scripts/` (host-side automation) | **Approved** | Optional (single design dispatch usually sufficient) |
| `todo/`, prose-only edits | **Not applicable** (no code review) | Not applicable |

The reason for the kernel/boot exclusion: same-model self-review (Claude reviewing Claude's own kernel code via subagent) systematically misses bugs that cross-MODEL adversarial review (Claude implementer + Codex GPT-5.5 reviewer) catches. Concrete incidents: the FPU context-switch bugs caught during the FPU context-switch review and the GOP MaxMode + overflow + ordinal findings caught during [TODO-02 GOP review](../../todo/01-boot-platform/TODO-02-uefi-hardening-secureboot.md#3-gop-resolution-auto-detection) (2026-04-27 re-review caught 5H+1M after the original same-model review had stamped the section verified).

Rule of thumb: if the file path matches `src/kernel/`, `src/boot/`, or `include/kernel/`, the four-dispatch Codex pipeline is non-negotiable. The subagent loop may run *alongside* the Codex pipeline (parallel quality work), but it never replaces a Codex dispatch.

## Updates and Maintenance

- When the superpowers plugin upgrades (e.g., 5.0.7 -> 5.1.0), re-walk this catalog and update verdicts. Skills can be added, renamed, or retired upstream. The plugin version is recorded in `~/.claude/plugins/installed_plugins.json`.
- New mandatory triggers go into both [CLAUDE.md "Mandatory Skill Triggers"](../../CLAUDE.md#mandatory-skill-triggers) and this file. The CLAUDE.md row is the agent-facing prompt; this file is the rationale + suppression rules.
- The `audit-hooks.sh` script does not currently inspect this file; add a check if/when a future drift class warrants it (e.g., a CLAUDE.md trigger row referencing a superpowers skill that has been retired upstream).
