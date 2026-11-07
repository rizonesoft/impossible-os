# Run Issues

Open problems with the run itself: flow, gates, cost. One running file, replacing the numbered capture versions on 2026-11-07, which remain in git history. Add a problem when you hit it, with what you observed, and fix it when you can. A fixed problem's entry is deleted, and the commit is its record. Design: [the run supervisor](../docs/specs/2026-11-07-run-supervisor-design.md).

## Open

- [ ] `SEQ-TEARDOWN` is a text match, so the guard CLI's `clear` has no enforcement of its own
  - `python3 .claude/hooks/run_phase_guard.py $(printf cl; printf ear)` is not refused. The owner is the CLI verb, which now only `run.sh stop` and a person should reach. Do not narrow the text match: three review rounds broke every narrowing.
- [ ] `section_commit_gate` reads a script shlex cannot tokenize as no commit at all
  - `git commit -m $'a\';b' -n` returns `(False, False)`. Fix by reading an untokenizable line naming `git commit` as a commit, measured for false refusals first.
- [ ] `section_commit_gate` step-8 test-wiring binds tests to the source BASENAME, which 434 of 516 kernel sources cannot satisfy
  - Settled by refusal data once kernel sections ship, then a same-commit test that registers a suite named for the staged basename, as a warning first.
- [ ] No review looks across the sections of a file: `codex-review-todo` and `quality-review-section` are never scheduled
  - Settled by running `codex-review-todo` once on a closed file after sections ship, and counting cross-section findings.
- [ ] Anything that depends on state from a previous boot is invisible to the suite and the smoke matrix
  - The first concrete need is the open item at `01-boot-platform/TODO-29:649`. A second-boot leg means restructuring `test-smoke.sh`, so it needs a design pass.
- [ ] Syscall arg counts are not in the ABI fingerprint
  - `render()` in `scripts/gen-user-abi.py` fingerprints names and numbers. `SYS_EXEC` is slated to go from `(path, len)` to `(path, argv, envp)`, which would land with the fingerprint unchanged. Fixing it closes `00-infrastructure/TODO-04` s41.
- [ ] The release flavor is not built at the section boundary
  - A parameter used only under `KERNEL_TESTS` breaks `KERNEL_TESTS=off` with `-Werror`, and only CI's release job sees it. A release-flavor build in the J1 chain is about 20 s.
- [ ] Does a single session hold up for days, and how much does compaction lose?
  - Untested. Watch the first runs. WSL restarting takes the heartbeat with it, so `run.sh start` has to be run again.
- [ ] Do the 36 `awaiting-answer` stamps pick up their answers?
  - All 30 questions are answered in `todo/answers.md`. Three answers mean stay as you are (Q14, Q17, Q24 report-only), so those need a plain Deferred reason or they read BLOCKED forever. Check with `sequencer_triage.py --blockers`.
- [ ] Do the logged gates matter?
  - Read `.claude/overnight/gate-log.jsonl` after a run. A gate nothing depended on gets deleted. A real defect it would have stopped puts it back to `enforce`.
- [ ] Does medium effort hold quality?
  - Writer and all agents moved to medium on 2026-11-06. No section has shipped since, so there is no comparison yet.
- [ ] Context growth is still the dominant token use
  - Compaction at 400K is the current lever. Compare tokens per shipped section across a change to `RUN_COMPACT_AT`. Also unsettled: trimming `CLAUDE.md`, and replay task sets before pinning `general-purpose` and `Explore` to Sonnet.

## Baselines

- Queue on 2026-11-07: 208 files need work, 17 are done, 2 are blocked.
- Refused calls, last headless run: 27 of 1,560 (1.7%), with `receiving-review-required` 10 and `build-offload` 6.
- Review depth across 831 sections: dispatches p50 5, p90 13, p99 32, max 62.
- `Build` runs: 37 success, 3 failure and 0 cancelled, of the 40 before the job split.
- Kernel image headroom: 66,500 bytes on the tightest section, down about 85 KB during the 2026-11-07 run.
