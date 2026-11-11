# Run Issues

Open problems with the run itself: flow, gates, cost. One running file, replacing the numbered capture versions on 2026-11-07, which remain in git history. Add a problem when you hit it, with what you observed, and fix it when you can. A fixed problem's entry is deleted, and the commit is its record. Design: [the run supervisor](../docs/specs/2026-11-07-run-supervisor-design.md).

## Open

Scheduled work is not kept here. On 2026-11-11 nine items moved to `00-infrastructure/TODO-08-automation-hardening.md` sections 33 to 41, where the plan schedules them. What stays is what the run cannot do for itself, and what can only be watched.

### Attended only: these edit the frozen launch files, which the run never touches

- [ ] The run transcript grows about 50 MB a day and a restart resumes the same session id
  - `run_launch` in `scripts/overnight/run-common.sh` falls back to a fresh session only when the transcript file is missing. Done when a failed resume, or a transcript over a set size, starts a fresh session from the cursor.
- [ ] Nothing restarts the heartbeat if it dies, and a busy session that ships nothing is never nudged
  - Done when the boot service or a timer respawns a dead `heartbeat.sh`, and the alert in `supervisor.py` keys on sections shipped per day, not on idleness.
- [ ] An expired Codex, Claude or `gh` login stalls every section with no distinct signal
  - Done when a daily probe of the three logins, started by `run.sh start`, raises the critical alert and names which one failed.
- [ ] `SEQ-TEARDOWN` is a text match, so the guard CLI's `clear` has no enforcement of its own
  - `python3 .claude/hooks/run_phase_guard.py $(printf cl; printf ear)` is not refused. The owner is the CLI verb, which only `run.sh stop` and a person should reach. Do not narrow the text match: three review rounds broke every narrowing.

### Watch: questions a number answers, not tasks

- [ ] Sections shipped per day. About 7 a day on 2026-11-11, against 1,649 open sections and a plan that grows; one push per section with a CI wait adds about 25 minutes a section. Read `supervisor-state.json` after a week before tuning anything.
- [ ] Does a single session hold up for days, and how much does compaction lose?
- [ ] Does medium effort hold quality? Writer and all agents moved to medium on 2026-11-06.
- [ ] Context growth is still the dominant token use.
- [ ] The first section reviewed as passes of the reviewer pin, and the first pushed once and watched to green, have not been watched end to end. Fix what they trip on in the skill or the gate.
- [ ] `test-tooling.sh` install-hooks lifecycle failed once in a pre-push run (2026-11-08, `pre-push status not ENABLED` with the sentinel present). If it recurs, capture `install-hooks.sh --status` and the sentinel path inside the test.

## Closed

- [x] Do the 36 `awaiting-answer` stamps pick up their answers? They did not; fixed 2026-11-11 (the classifier reads `todo/answers.md`, 36 stamps and 128 items reopened).
- [x] Do the logged gates matter? No; all 22 were deleted on 2026-11-09.
