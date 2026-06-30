---
name: overnight-todo-runner
description: Run an Impossible OS TODO file to completion overnight. One slice = one section ship through /implement-todo-section + /review-todo-section. Use when the user says "run overnight", "carry on with todo/...", "overnight implement TODO-XX", or otherwise asks to keep working a TODO file end-to-end without stopping between sections. Stop ONLY on file exhaustion, explicit pause, or blocked-with-XREF.
---

# Overnight TODO Runner

> **External-Reviewer Contract:** This skill invokes Codex indirectly through child section/file workflows (`implement-todo-section`, `review-todo-section`, and `complete-todo-file`). Every Codex finding goes through `superpowers:receiving-code-review` (verify at file:line or source section, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

> Slice = section ship. Stop is a checkpoint, not the end. The Stop-hook returns exit 2 while state is active; respect it -- start the next [ ] section instead of final-answering.

## Use This Skill When

The `UserPromptSubmit` hook activates state ONLY on an explicit imperative phrase. Bare mentions of "overnight" + "todo" in passing -- meta-discussion, pasted hook output, documentation drafts -- do NOT activate. The recognised phrases (case-insensitive):

- `start overnight` / `start the overnight`
- `begin overnight` / `begin the overnight`
- `run overnight` / `run the overnight` / `run an overnight`
- `kick off overnight` / `kick off the overnight`
- `launch overnight` / `launch the overnight`
- `resume overnight` / `resume the overnight`
- `carry on overnight` / `carry on with overnight`

Example activation prompts:

> "Run overnight on todo/01-boot-platform/TODO-09-cpu-boot-sequencing.md and todo/01-boot-platform/TODO-10-bare-metal-hardening.md"
> "Kick off overnight TODO implementation on todo/02-kernel-core/TODO-20-eif-full-implementation.md"
> "Resume the overnight run"

Activation also exits early if the prompt contains hook-output markers (`[overnight-guard]`, `overnight-guard.py`, `Stop hook feedback`, `Stop sentinel`, `session-resume]`) -- this prevents pasting the guard's own output back into chat from accidentally re-arming it.

Do NOT use for:

- One-off section ships -- use `/implement-todo-section` directly.
- TODO-file authoring -- use `/create-todo` and `/todo-pipeline` first.

## Activation

The `UserPromptSubmit` hook auto-activates when the prompt matches the patterns above. **Every TODO path mentioned in the activation prompt becomes part of the queue**, in order of first appearance: the first path becomes the active TODO, the rest queue behind it. Example:

> "run overnight: todo/01-boot-platform/TODO-09-cpu-boot-sequencing.md, then todo/01-boot-platform/TODO-10-bare-metal-hardening.md, then todo/01-boot-platform/TODO-11-interrupt-timer-arch.md"

Writes `.claude/state/overnight-run.json` with `todo_file = TODO-09...` and `queue = [TODO-10..., TODO-11...]`.

Manual control:

```sh
# Start fresh (first arg is the active TODO; the rest queue behind it):
python3 .claude/hooks/overnight_guard.py start todo/01-boot-platform/TODO-09-...md todo/01-boot-platform/TODO-10-...md

# Append more TODOs to an active run:
python3 .claude/hooks/overnight_guard.py enqueue todo/01-boot-platform/TODO-12-...md

# Pause on error (halt advancement, unblock Stop so the agent can diagnose):
python3 .claude/hooks/overnight_guard.py pause "what failed and why"

# Resume after fix + reverification:
python3 .claude/hooks/overnight_guard.py resume

# Check state:
python3 .claude/hooks/overnight_guard.py status

# Clear (intentional end of run):
python3 .claude/hooks/overnight_guard.py clear "reason"
```

## Multi-TODO Queue

When the current TODO's Implementation Order is fully `[x]`, the next `PostToolUse` or `Stop` tick auto-advances:

1. Moves the exhausted TODO to `completed[]`.
2. Pops the next path from `queue[]` and sets it as `todo_file`.
3. Emits a `[overnight-guard] X exhausted; advancing to Y` line so the agent knows to switch focus.
4. Subsequent ticks operate on the new TODO.
5. Queue entries that are *already* fully shipped at the time of advancement are silently skipped (added to `completed`, not worked).

The run ends only when both conditions hold: the current TODO is exhausted AND the queue is empty. At that point the guard clears state and `Stop` returns 0 (no block).

You can also append a single completion step between TODOs by invoking `Skill(complete-todo-file)` on the exhausted file before the next tick -- the loose-end sweep + closure commit fits naturally between auto-advancements.

## Workflow (per section)

1. **Pick the next [ ] row in the active TODO's Implementation Order table.** If two are eligible, prefer the one with no upstream `XREF` dep on a still-[ ] section elsewhere. Surface the choice in the section-start update.
2. **Skill(implement-todo-section) on `<path> section N`.** This is the full canonical pipeline -- design + adversarial + consistency + perf Codex, fix loop, build, smoke test (when boot paths are touched), unit tests, validate, commit, push. `section_commit_gate.py` enforces the [x] flip ties to a Verified stamp.
3. **Skill(review-todo-section) immediately after the section-ship commit.** Mandatory per CLAUDE.md "Mandatory Skill Triggers". Three Codex dispatches (adversarial + consistency + perf) + stamps Verified. Hook `section_review_required.py` will BLOCK Edit/Write/non-review Skills until this runs.
4. **Reread the TODO file in full** before picking the next section -- gap-audit XREFs or scope notes added during step 2 may have grown the file.
5. **Move to the next unblocked [ ] section.** Do not final-answer between sections; the `Stop` hook will block (exit 2) and remind you.

## Stop Conditions

End the overnight run ONLY when one of these holds. Any other Stop will be blocked by the hook.

- **Exhausted.** Every row in Implementation Order is [x]. The hook auto-clears state on the post-commit pass; then run `Skill(complete-todo-file)` for the loose-end sweep + closure commit, and final-answer with the closure summary.
- **User paused.** The user's prompt matched a pause phrase ("pause the overnight", "stop the overnight", etc.) -- hook cleared state. Final-answer with a one-line status of where the run stopped + the next [ ] section.
- **All remaining sections blocked.** Every remaining [ ] section has a concrete blocker (missing Inputs, prerequisite TODO not shipped, hardware unavailable). Before stopping, file each as Deferred with a named XREF to the owning TODO/section, then clear state manually:

  ```sh
  python3 .claude/hooks/overnight_guard.py clear "all remaining sections blocked; deferred with XREFs"
  ```

## Guardrails

- **Never silence the guard to short-circuit a slow section.** If a Codex dispatch or build is slow, wait for it -- do not `clear` state to escape the loop.
- **Never skip /review-todo-section.** Section-ship without the stamped review is the `feedback_never_skip_review` failure mode the guard exists to prevent.
- **Never mark a section [x] in Implementation Order without the Verified stamp.** The post-impl review writes the stamp; the IO table only flips after.
- **Do not edit the TODO's `## Notes` block as a review-evidence dump** -- 3-6 short bullets per `feedback_todo_notes_brevity`.
- **No corner cutting on the per-section pipeline.** The `/implement-todo-section` skill is not negotiable; running it overnight does not relax design + adversarial + consistency + perf.
- **Blocked != hard.** Before declaring a section blocked, confirm Inputs paths exist and the named prerequisite is genuinely [ ] in its owning TODO. "Hard to think about" is not blocked.

## HALT ON ERROR (non-negotiable)

Overnight running does NOT lower the testing bar. The opposite: with the agent looping unsupervised, the harness's existing test gates become load-bearing. **Stop on first hard failure; do not "make progress" by skipping the failing step.**

Halt conditions -- any one of these means **pause the run immediately**, diagnose, fix root cause, then `resume`:

- **Build failure.** `bash scripts/build.sh` did not show `=== BUILD OK ===`. Read `tail -50 build/build.log`; do not retry blind.
- **Unit test failure.** `bash scripts/test.sh` reports any FAIL (not SKIP, not PENDING -- FAIL). Re-run the specific suite first to rule out transient flake; if it reproduces, halt.
- **Smoke test failure.** `bash scripts/test-smoke.sh` did not show `Boot complete` + `C:\>`. Read `build/smoke-test.stripped.log`; the boot path is broken and the next section will inherit the same broken kernel.
- **Codex review fix loop stuck.** Round 3 of `codex-adversarial-review-section` still returns High/Critical findings that resist fixes. Halt; this section needs human judgment, not another fix attempt.
- **Hook BLOCK that can't be cleared by the documented opt-out.** Any PreToolUse hook in `.claude/hooks/` refusing the next Edit/Write with a BLOCK reason that doesn't match a legitimate opt-out condition (see `feedback_codex_design_review_mandatory`).
- **Section_commit_gate refuses the commit.** The `[x]` flip in Implementation Order doesn't match a stamped review, or the post-impl review has not happened. Halt and run the review BEFORE looking for workarounds.

The mechanism:

```sh
python3 .claude/hooks/overnight_guard.py pause "smoke test crashed at boot phase 1: PF at RIP=0xFFFFFFFF80012345 -- post-commit-smoketest hook reports VBox+KVM both panic"
```

While paused:
- `Stop` hook returns 0 (no block) so the agent can final-answer with the diagnosis.
- `SessionStart` hook surfaces the pause reason prominently to any new session.
- `maybe_advance` is disabled -- the queue does NOT advance, even if the current TODO appears exhausted on disk.

To resume after the root cause is fixed and re-verified:

```sh
python3 .claude/hooks/overnight_guard.py resume
```

Forbidden "fixes" -- these violate `feedback_no_bandaids`, `feedback_fix_root_causes`, `feedback_test_fail_fix_code_not_test`, `feedback_no_corner_cutting`:

- Marking the failing section `[ ]` -> Deferred without filing a concrete blocker with XREF to the responsible TODO.
- Adding a skip list, conditional `TEST_SKIP`, or platform-specific guard to make the test pass.
- Widening an assertion to accept the broken value.
- Disabling the post-commit smoke test hook.
- Editing the TODO's Notes block to claim the section is shipped when tests don't pass.
- Resuming the run without rerunning the previously-failing build/test/smoke check.

`feedback_just_do_it` does NOT extend to skipping verification of failures. Diagnosis is the work, not an interruption to the work.

## State File

`.claude/state/overnight-run.json` (under `.claude/state/`'s existing ignore rules). Schema:

```json
{
  "active": true,
  "todo_file": "todo/01-boot-platform/TODO-09-cpu-boot-sequencing.md",
  "queue": [
    "todo/01-boot-platform/TODO-10-bare-metal-hardening.md",
    "todo/01-boot-platform/TODO-11-interrupt-timer-arch.md"
  ],
  "completed": [
    "todo/00-infrastructure/TODO-01-developer-tooling-stack.md"
  ],
  "source": "UserPromptSubmit",
  "started_at": "2026-05-23T12:00:00+00:00",
  "updated_at": "2026-05-23T12:30:00+00:00"
}
```

`active: false` or absent file = guard inactive (no reminders, no Stop block, no SessionStart resume).

## Surviving the 5-Hour Usage Limit

Claude Code's account-level usage cap will terminate the current conversation when it trips. The conversation cannot be revived in place. What survives the limit is the state file and the workflow around it:

1. **Conversation dies.** The harness stops accepting tool calls. Any in-flight Codex dispatch is interrupted (Codex itself uses a separate budget but the foreground Bash call returns).
2. **State persists.** `.claude/state/overnight-run.json` is unaffected. Current `todo_file`, queue, completed list, and started timestamp survive.
3. **New session resumes automatically.** When you (or a scheduler) start a fresh Claude Code session in this repo, the `SessionStart` hook fires `overnight_guard.py session-start`, which detects active state and emits a system reminder:
   - `[overnight-guard: session-resume] Active overnight run detected ...`
   - lists current TODO + remaining queue + completed count
   - tells the agent to reread the current TODO, find the next `[ ]` section, and continue the loop.
4. **Agent picks up.** The fresh session has no conversation memory of prior work, but the state file + the SessionStart reminder + the TODO file's own `Verified:` stamps and `[x]` rows give the agent enough context to identify exactly where to resume.

What does NOT work automatically:

- **Auto-restarting the Claude CLI itself.** The harness has no built-in scheduler that survives its own death. You need an OS-level mechanism (cron, systemd-timer, Windows Task Scheduler) to launch a fresh `claude` invocation at the reset time. Example sketch (pattern only -- verify the exact CLI invocation for your install):

  ```sh
  # crontab -e
  # Resume the overnight run at 03:00 daily IF state is active.
  # Adjust to the hour after your usage window resets.
  0 3 * * * cd /home/derickpayne/impossible-os \
            && [ "$(python3 .claude/hooks/overnight_guard.py status | head -c 12)" != "[overnight-g" ] \
            && claude --print "resume the overnight TODO run" >> /tmp/claude-overnight.log 2>&1
  ```

  The `--print` flag (and exact CLI invocation) depends on your Claude Code version; verify before relying on it. The status-check guards against starting a session when no run is active.

- **Bypassing the account quota.** The 5-hour window is account-level. A fresh session immediately after the reset has the full budget; a session started during the cooldown still hits the same wall. The cron job only saves wall-clock waiting -- it doesn't get you "extra" hours.

- **Mid-section recovery.** If the limit hits during `/implement-todo-section` partway through a section, the next session resumes by *reading the TODO + git state and deciding what to do* -- it does NOT auto-resume the partial Codex dispatch. The agent should check: is there an in-flight section with `[x]` rows in the implementation but no `Verified:` stamp? If so, run `/review-todo-section` on it first to either confirm or downgrade, then continue.

In practice: most overnight runs that cross the limit will complete the section that was in flight (Codex calls are independent and the in-progress section gets either committed-and-stamped or rolled back by the section-commit gate), then die at the next `Stop`. The new session sees the clean state and continues.

## Hook Wiring

Entries in `.claude/settings.json` invoke `.claude/hooks/overnight_guard.py`:

- `UserPromptSubmit` -> `overnight_guard.py prompt` (activate/pause; extracts the full queue from one prompt)
- `SessionStart` -> `overnight_guard.py session-start` (resume reminder for a fresh session -- the rate-limit-reset path)
- `Stop` -> `overnight_guard.py stop` (exit 2 sentinel while active; auto-advances on exhaustion, clears state when the queue is empty)

The Stop hook returns exit 2 to keep Claude running; the harness surfaces the stderr text as a system reminder. The SessionStart hook returns 0 -- it does not block, just informs.

> **Note on `PostToolUse`:** the original Officium-style design also wires PostToolUse for mid-tool reminders. That entry was denied by Claude Code's auto-mode classifier on first try. The two hooks above are sufficient: SessionStart handles cross-session continuity, Stop is the actual loop-keeper. If you want the mid-tool reminder, manually append the entry to `.claude/settings.json` under the `PostToolUse` `matcher: ".*"` block: `{ "type": "command", "command": "python3 .claude/hooks/overnight_guard.py post-tool", "timeout": 5 }`.

## Relationship to /implement-todo-section, /review-todo-section, /complete-todo-file

This skill is a **driver loop**, not a replacement. It calls the existing skills in sequence and prevents the Stop event from ending the conversation between iterations. Every existing gate (Codex pipeline, section_commit_gate, section_review_required, smoke-test post-commit hook) keeps running unchanged -- the overnight guard just ensures the loop continues until the file is genuinely done.
