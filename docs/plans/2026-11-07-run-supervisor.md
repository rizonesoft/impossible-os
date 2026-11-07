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
- Done when: each gate in `log` mode passes the call and writes one line to `.claude/overnight/gate-log.jsonl`, each in `enforce` mode still refuses, and an unreadable modes file means `enforce`.

## Stage 5: One session, stop guard, heartbeat

- `scripts/run.sh start|attach|status|stop`: one Claude session in tmux. Subagents and Codex stay separate.
- Stop guard: the existing Stop hook, which no longer treats a verified rollover as a reason to end.
- `scripts/overnight/heartbeat.sh`: every 5 minutes, restart a dead session with `--resume` on its own session id, nudge an idle one, and tell a stalled one to diagnose.
- `scripts/overnight/supervisor.py`: health, and undo of a machinery commit, only while the session is idle.
- Compaction at 400K through `CLAUDE_CODE_AUTO_COMPACT_WINDOW`.
- One review cap of 42 rounds for all code. A clean round exits.
- Done when: the fixtures show each heartbeat action, a stopped or finished run is not restarted, and the Stop hook holds until fixpoint.

## Stage 6: Remove what is left

- Delete the systemd machinery: `arm-sequencer.sh`, `overnight-arm.sh`, `overnight-launch.sh`, `stream-report.py`, `run-outcome.py`, `deadline-check.sh`, `noship-update.sh`, `reset-breaker.sh`, `token-probe.sh`, `run-liveness.sh`, `attended_repair_guard` and their tests.
- Delete the rotation code: the `rollover-wip` verb and `section-checkpoint.py`, if nothing else needs them.
- Rewrite the CLAUDE.md sections on hooks, attended repair and the shared tree. With one session there is no shared tree.
- Replace the capture directories with `todo/RUN-ISSUES.md`, moving the open v23 items, and repoint the 59 referencing files and 23 TODO files.
- Delete the `close-canary-run` skill, `capture-open-findings.py`, the control-plane allowlists and the canary doctrine.
- Check the 121 unchecked lines in old capture versions for live work before deleting them.
- Done when: `grep -ril canary` finds only git history and this plan's own record, and lint, the runner suite and the tooling pack are green.

## Stage 7: Start a run and watch it

- `bash scripts/run.sh start`, then `attach`. Watch sections shipped and gate warnings, and talk to it directly.
- After a watched run with no sign a `log` gate was needed, delete those gates' code. That is the point at which the 70,000 lines actually shrink.

## Decisions

| Date | Decision | By |
|---|---|---|
| 2026-11-07 | Keep product checks, cut the rest, in stages on the existing code | operator |
| 2026-11-07 | The run repairs its own machinery with auto-undo | operator |
| 2026-11-07 | No spend cap; alert only | operator |
| 2026-11-07 | Emulators for now, always developing toward real hardware | operator |
| 2026-11-07 | One session in tmux replaces the separate headless process | operator |
| 2026-11-07 | Compact at 400K; about the same token use as rotation | operator |
| 2026-11-07 | A stop guard and a heartbeat stay; the watchdog goes | operator |
| 2026-11-07 | Subscription billing, so health reports no dollars | operator |
| 2026-11-07 | One review cap of 42 rounds for all code; a clean round exits | operator |
