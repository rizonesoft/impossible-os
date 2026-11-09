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

- Deleted the systemd machinery: `arm-sequencer.sh`, `overnight-arm.sh`, `overnight-launch.sh`, `run-outcome.py`, `deadline-check.sh`, `noship-update.sh`, `reset-breaker.sh`, `token-probe.sh`, `run-liveness.sh`, `pre-arm-check.sh`, `run-status.py`, `overnight-monitor.sh`, `segment-sync.sh`, `lifecycle-unblocked.sh`, `browser-lane-enabled.sh`, `parse-usage-limit.py` and their tests.
- Replaced the capture directories with `todo/RUN-ISSUES.md`. The 46 versions stay in git history. Of their unchecked lines, 46 had no verdict and no later re-filing. They were mostly complaints about gates this work removes, or reasoning notes. The two still live, ABI arg counts and the release flavor, moved across.
- Deleted the `close-canary-run` skill, `capture-open-findings.py` and the canary allowlist, and rewrote the CLAUDE.md section as The Run.
- `attended_repair_guard` stays, unlike the first draft of this plan: it is a safety check, and a second session can still open beside a run.
- Dated `canary` mentions in comments stay as history. Only operative references went. The `rollover-wip` verb, `stream-report.py` and the metrics scripts stay until stage 7 shows whether anything still needs them.

## Stage 7: Start a run and watch it

- `bash scripts/run.sh start`, then `attach`. Watch sections shipped and gate warnings, and talk to it directly.
- After a watched run with no sign a `log` gate was needed, delete those gates' code. Also delete the rotation and report leftovers named in stage 6. That is the point at which most of the 70,000 lines actually go.
- Done 2026-11-09, after a run that shipped 22 sections. Deleted: the 17 `log`-mode hooks, two helpers only they used (`_review_pipeline_passthrough.py`, `_state_root.py`), the five `SEQ-*` process codes in `run_phase_guard.py` (phase order, lifecycle routing, R1, the armed-run redirect, and the non-frozen half of the control-plane refusal), `gate-modes.json` and the log wrapper in `gate.py` (which now only lists the frozen files), the `mark-rotation`, `relifecycle`, `review-resolved` and `rollover-wip` verbs, the rollover pending flag, `stream-report.py`, `metrics-report.py`, `section-cost-report.py`, and their tests. Kept: every safety refusal, the stop guard, the verified checkpoint (`rollover`) and its two-refusals-then-park rule. Found on the way: the tmux launcher had dropped the units' ChromeMCP-off environment; it is restored and tested.

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
