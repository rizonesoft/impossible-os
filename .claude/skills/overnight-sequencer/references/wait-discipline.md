# Wait discipline -- the measurements behind each rule

Read on demand from the `overnight-sequencer` "Wait discipline" section. The SKILL.md text carries the operative
command shapes; this file carries the measured costs that produced them. `codex_wait_discipline.py` BLOCKs a
short poll in the headless run, so the enforcement does not depend on this file being read.

## Why one blocking call, never a poll burst

A sleeping shell costs ~0 model tokens -- the model emits one Bash call and is idle while `sleep` runs -- so holding
the session open during a review is free. The expensive antipattern is MANY separate poll calls with narration
between them.

- **2026-07-02 run:** 531 `Holding...` turns + 610 re-polls.
- **run-20260719-022200:** the short-poll loop cost 51 polls x ~350K cached tokens, about **27% of that session's
  Bash calls**. R3 (2026-07-19) replaced it with one long call.

The waiter exits the moment every verdict lands, so a long `--max` bound is FREE when the review is already done.
`--max 540` plus the Bash tool parameter `timeout: 600000` is mandatory in the headless run: the tool's ~120s default
kill would otherwise end the wait early (B1).

## Waiter exit codes

- **0** -- DONE, prints the verdict tail.
- **3** -- STILL RUNNING (reports bytes + last-growth age). Re-issue the SAME long call; the review runs detached and
  nothing is lost.
- **4** -- STALE (no growth for `--stale-secs`, likely hung). Re-dispatch THAT log only and keep waiting on the rest
  (B2).

It is `--max <secs>`; a bare trailing number is REJECTED as a stray arg.

## Why the session never exits to wait

There is no `wait`/`wake` verb and no watcher -- that apparatus deadlocked the runner and was removed. The only
session exits are a verified `rollover` (context hygiene; the watchdog relaunches), external death (crash /
usage-limit; watchdog relaunches), or an oracle-verified `fixpoint`.

## Why the envelope must be `--todo` AND `--section` scoped (E3)

The broker manifest accumulates across sections and runs, so an unscoped `review-envelope.py` read pulls a PRIOR
section's stale review -- and `--todo` alone is not enough: a section's wave silently ingested an EARLIER section's
legs from the SAME todo (observed 2026-08-14, TODO-10 section 26 pulling section 25's consistency+perf legs). Pass
`--section <n>` as well; the broker records the section it parses from the dispatch prompt's first line.

## Why never to glob the Codex session files (E2)

The `.out` artifact plus the envelope IS the authoritative review body. Never glob `~/.codex/sessions/**/*.jsonl` for
the "full" review: the thread-id does not map to a session filename, so that glob returns an unrelated foreign
session. Measured: a Conclave run was misread as the verdict.

## Why a crashed leg is re-dispatched alone

Codex `app-server exited unexpectedly, rc=1` crashes are common and produce no verdict. Re-running the full 3-leg
bundle when one leg crashed was a top measured token sink: **16 crashes / 5+ full re-dispatches in one section**
(2026-07-12). The envelope reports `crashed` (completed-but-crashed kinds) and `needs_redispatch` (exactly the legs to
re-run); envelope exit 0 means `all_clean`. Re-dispatch precisely `needs_redispatch` and reuse the already-clean legs'
artifacts as-is.

## Convergence gate and round counter -- scope detail

Per-kind fingerprint scope: adversarial / perf / re-adversarial fingerprint SOURCE only; consistency / design
fingerprint SOURCE + TODO. A docs/TODO-only fix therefore converges the source-only kinds, and one changed file never
re-triggers ALL kinds. Fail-open (unknown kind / git error -> redispatch): the gate can only skip a redundant review,
never suppress a needed one.

The round counter is STALL detection, never a fixed round cap -- a productive review runs as long as it keeps finding
new Critical/High. Exit 2 = CAPPED means K consecutive no-new rounds or the 30-round infinite-loop ceiling.

For rounds >= 4 the file:line verification goes through ONE `review-evidence-mapper` dispatch rather than inline
re-reads: the 2026-07-12 run re-read `task.c` ~31x. The `agent_result_cache` is keyed on the scoped evidence set
rather than the volatile round prompt, so an unchanged-content map is served from cache.

## Backgrounding a wrapped command: use the flag, never `&` as well

A backgrounded `run-artifact.sh` (or any wrapped build/test) reports its verdict in `.claude/state/last-artifact.json` (`state`/`exit`), NOT in the harness exit code. Background it with the Bash `run_in_background` flag alone. Adding `&` INSIDE an already-backgrounded call re-creates the exact hazard the flag removes: the tracked process becomes the LAUNCHER, which returns `completed (exit code 0)` within seconds while the suite is still running, so reading that notification certifies a suite that has not started its first test (observed 2026-08-11). Read `state`/`exit` from the envelope, and never put `&` in the command body.
