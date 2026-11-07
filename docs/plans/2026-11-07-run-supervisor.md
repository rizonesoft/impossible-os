# Run Supervisor Campaign

Design: [docs/specs/2026-11-07-run-supervisor-design.md](../specs/2026-11-07-run-supervisor-design.md). Each stage ships on its own, leaves a working runner, and is reversible with `git revert`. Every stage gets its tests, a Codex review, and a green runner suite before push.

## Stage 1: Starting always works

- Delete the canary gate in `arm-sequencer.sh` (`--record-canary`, the stamp, `--force` as a canary bypass). `--force` stays accepted and does nothing, so nothing that calls it breaks.
- Delete the closed-capture refusal and the unverdicted warning.
- Non-primary worktree: re-run from the primary automatically instead of refusing.
- `pre-arm-check.sh`: a failing suite or selftest warns and starts, because the run can repair it. No token, no toolchain and a matching skill deny still refuse, because a run cannot work without them.
- Done when: an arm from a dirty machinery change, a stale stamp or a closed capture file starts. A missing token still refuses.

## Stage 2: The run does not end on a fault

- `deadline-check.sh`: a no-ship streak no longer ends the run. Only `--hours` does.
- `overnight-launch.sh`: a breaker trip backs off and alerts, and stays armed.
- A no-ship streak or a breaker trip launches a repair segment with a prompt that says why it was started.
- Done when: fixture runs show 5 no-ship segments leaving the run armed with an alert sent and a repair prompt selected, and `--hours` and fixpoint still end it.

## Stage 3: A failure stops one item

- Attempt counter per section in `sequencer-run.json`.
- After 2 failed attempts, park the section with its reason and move on.
- `cursor` no longer refuses on `rollover_refused` or R1. The rollover check still refuses a VERIFIED verdict on red receipts. A section that cannot go green is reverted and parked.
- Done when: a fixture section that always fails is parked and the cursor reaches the next one, and a green receipt is still never written over a failed run.

## Stage 4: Process gates become warnings

- Add `.claude/gate-modes.json` and one helper the hooks call instead of exiting 2.
- Move the process and cost hooks in the design's table to `log`. Product, safety and style stay `enforce`.
- `SEQ-CONTROL-PLANE` and `receipt_surface_guard` become `log`, leaving only the frozen supervisor list refused.
- Done when: each gate in `log` mode passes the call and writes one line to `build/gate-log.jsonl`, each in `enforce` mode still refuses, and an unreadable modes file means `enforce`.

## Stage 5: Supervisor, auto-undo and health

- Split the frozen supervisor out of `overnight-launch.sh`, and move the policy parts out of it.
- Record machinery commits, and revert them when the next 2 segments ship nothing.
- Write `build/run-health.json` and send the two alerts.
- Attended repair from a worktree: rewrite the CLAUDE.md section down to the new, simpler rule.
- Done when: a fixture machinery commit followed by 2 empty segments is reverted and alerted, and a revert that itself fails alerts and leaves the run going.

## Stage 6: Remove what is left

- Replace the capture directories with `todo/RUN-ISSUES.md`, moving the 8 open v23 items, and repoint the 59 referencing files and 23 TODO files.
- Delete the `close-canary-run` skill, `capture-open-findings.py`, the control-plane allowlists and the canary doctrine, and trim CLAUDE.md to match.
- Check the 121 unchecked lines in old capture versions for live work before deleting them.
- Done when: `grep -ril canary` finds only git history and this plan's own record, and lint, the runner suite and the tooling pack are green.

## Stage 7: Start a run and watch it

- One command. Watch sections shipped, cost per section and gate warnings.
- Fix live, from a worktree, whatever goes wrong.
- After a watched run with no sign a `log` gate was needed, delete those gates' code. That is the point at which the 70,000 lines actually shrink.

## Decisions

| Date | Decision | By |
|---|---|---|
| 2026-11-07 | Keep product checks, cut the rest, in stages on the existing code | operator |
| 2026-11-07 | The run repairs its own machinery with auto-undo | operator |
| 2026-11-07 | No spend cap; alert only | operator |
| 2026-11-07 | Emulators for now, always developing toward real hardware | operator |
