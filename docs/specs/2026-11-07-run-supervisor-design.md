# Run Supervisor: Start It Once, Let It Ship

**Goal:** one command starts a run, the run keeps shipping sections until the roadmap is done, and no single fault stops it. Decided with the operator on 2026-11-07. Campaign: [docs/plans/2026-11-07-run-supervisor.md](../plans/2026-11-07-run-supervisor.md).

## Why the current design fails

The machinery mixes two kinds of check. Product checks ask whether the OS works. Process checks ask whether the agent followed the procedure. Process checks now outnumber product checks and cause most of the wedges.

Measured on 2026-11-07:

- 368 commits touched the machinery since 2026-08-01, against 178 touching the OS.
- The control plane is about 70,000 lines: 92 hook scripts, 84 hook bindings and a 22,584-line tooling test file.
- An arm has 14 ways to refuse or exit. 109 files mention the canary.
- The latest run cost $670 over 20 segments and shipped no section, because one permission rule refused one skill. Every segment reported a healthy checkpoint.
- Only 2 of 227 files read BLOCKED, so parked work is not what limits throughput.

Three designs make any fault fatal:

- `deadline-check.sh:67` ends the whole run after 2 segments ship nothing.
- `overnight-launch.sh:489` disarms on a breaker trip. Its own docstring (`run-outcome.py:28`) still says the run stays armed.
- `run_phase_guard.py:2034` sets `rollover_refused`, and `cursor` then refuses to move to another section. One bad section holds the queue.

## The rules

1. **A failure stops one work item, never the run.** The item goes back in the queue with its reason, and the run takes the next one.
2. **Starting is one command, and it always starts.** It refuses only what makes running impossible: no token, or no toolchain.
3. **Product checks stay, and they only stop code reaching `main`.** Build, unit tests, the boot matrix, host gates and Codex review.
4. **Safety checks stay.** They prevent real damage: destructive git, `--no-verify`, a fixture identity on `main`, Codex write access in a run, and the run tearing itself down.
5. **Process checks become logged warnings, then go.** A warning that keeps firing is data. A refusal that wedges a run is not.
6. **The run decides.** Already in place: it records the choice and its reversal cost in `todo/answers.md`, and only unrecoverable data loss, credentials, money, shared infrastructure or a wrong-looking calculation parks.
7. **The run repairs its own machinery, and a small frozen supervisor can undo that.** A person can also fix things live from a separate checkout.
8. **One number says whether it is healthy: sections shipped per day.** The run is on a subscription with no spend cap, so health reports no dollars.
9. **One session.** Everything runs in one session you can watch and talk to. Subagents and Codex stay separate. The saving always came from resetting context, not from being a separate process: compacting at 400K uses about 0.92x today's cache reads, on the last run's numbers.

## Classifying what exists

| Class | Examples | After this work |
|---|---|---|
| Product | build, test and smoke receipts at rollover; pre-push CI parity and host gates; that a Codex review happened before a ship stamp | stays blocking at commit, push and rollover, and only for that item |
| Safety | `SEQ-TEARDOWN`, `SEQ-WORKTREE`, `SEQ-CODEX-WRITE`, `--no-verify` refusal, push identity, `attended_repair_guard` | stays blocking |
| Process | `SEQ-PHASE`, `SEQ-LIFECYCLE`, `SEQ-R1`, `SEQ-REDIRECT`, `agent_dispatch_required`, `design_review_required`, `step5_quality_gate`, `review_dispatch_gate`, `receiving_review_required`, `phase1_evidence_gate`, `broker_dispatch_required`, `review_kind_literal_required`, `skill_step_block`, `section_review_required`, `codex_wait_discipline` | logged warning, then deleted after a watched run |
| Cost routing | `agent_coverage_gate`, `read_cache_block`, `websearch_offload_gate`, `fork_context_guard`, `agent_result_cache` pre, the `build_offload_reminder` refusal (its rewrite stays) | logged warning, then deleted if spend does not move |
| Style | dash, section-ref, shorthand and TODO-format hooks | unchanged: refusing one edit costs one edit and stops nothing else |
| Start-up | canary stamp, closed capture file, unverdicted findings, non-primary worktree | deleted, or resolved automatically |

A reviewer's skill can still say to run a review or apply reception. What changes is that forgetting a step costs a logged warning, not a wedge, and the ship check still requires that a review happened.

## The moving parts

**Gate modes.** Every process and cost hook reads one data file, `.claude/gate-modes.json`, that says `enforce` or `log`. Flipping a gate is a one-line data change that takes effect on the next call, is trivially reversible, and lets a run or a person turn off a gate that is wedging. Code is deleted only after a watched run shows nothing depended on it.

**Work-item failure.** The cursor records attempts per section. After 2 failed attempts (a refused ship, a failing suite, a review loop at its cap), the section is parked with its reason and the cursor moves on. Parked-on-failure work stays visible and retries once per pass, because a later fix often clears it. `rollover_refused` stops holding the queue.

**The session.** `scripts/run.sh start` runs Claude in tmux with `CLAUDE_CODE_AUTO_COMPACT_WINDOW=400000`, and `attach` opens it. Compaction replaces relaunching, so the disk is the memory: the cursor, git and the TODO files. Systemd, the launcher and token drop-ins all go.

**The stop guard.** The existing Stop hook refuses a stop until fixpoint, an operator stop, or a fully blocked queue. A verified rollover no longer ends the session; it goes straight to the next section.

**The heartbeat.** `scripts/overnight/heartbeat.sh` checks from outside every 5 minutes. A dead session restarts with `--resume` on its own session id, backing off if it keeps dying. A session with no transcript activity for 45 minutes is nudged, which also covers the end of a usage-limit wait, since automatic waiting stops after three continuations. A stalled one is told to diagnose. It acts only on faults. The old watchdog relaunched as its normal way of working.

**The supervisor.** `scripts/overnight/supervisor.py`, called by the heartbeat, writes health and raises alerts. Frozen files, which a run may not edit: `run.sh`, `run-common.sh`, `heartbeat.sh`, `supervisor.py`, `gate.py`, `run_phase_guard.py` and `settings.json`. A run can still set a process gate to `log`, because that data file is not frozen and cannot reach a product or safety check.

**The run ends only when** the oracle says fixpoint or an operator stops it. Nothing else ends it.

**Self-repair and auto-undo.** The run may edit the machinery, and the runner test suite still gates those commits. If 3 hours pass with no commit outside the machinery and the session is idle, the supervisor reverts those commits, pushes and alerts. Only when idle, because the session shares the working tree. Producing anything clears them.

**Review depth.** One cap of 42 rounds for all code, the highest count that clearly paid, and a clean round exits. Across 831 sections the p99 is 32 dispatches, so the cap binds only on outliers. The rule to change structure by round 3 stays, because that is what stops a spiral.

**Findings.** One running file, `todo/RUN-ISSUES.md`, replaces the 46 versioned capture files. A problem is fixed and its entry removed, with no close-out ritual. The old versions stay in git history.

**Health.** `.claude/overnight/run-health.json`, written by the heartbeat: sections shipped today, hours since the last ship, gate warnings per class, and machinery commits and reverts. It alerts when nothing ships for 6 hours.

## Hardware

Testing stays on emulators for now, and development always aims at real hardware. The bare-metal gotchas and code rules stay in force, and each section still records what only real hardware can prove. A hardware check never parks the code around it: the code ships, and the check joins a running list for when a machine is available.

## What this does not promise

- A green run is evidence, not proof. Emulators miss real-hardware bugs, and the first bare-metal boot will find some.
- Finishing the roadmap is not the same as rivalling Linux and Windows, which is also a matter of drivers and applications built over decades.
- Removing process gates removes some nets. The bet, based on the failures measured above, is that product checks catch what matters. Health and warning counts are how we find out if that bet is wrong.
- Roughly 1,570 sections remain, so this takes a long time. Earlier dollar estimates are withdrawn: `cost-summary.py` reports relative sizing at old list rates, not a bill.
- A single session running for days is untested here. WSL restarting takes the heartbeat with it, so `run.sh start` has to be run again, which resumes the work.
