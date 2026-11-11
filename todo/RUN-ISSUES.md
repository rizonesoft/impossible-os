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
  - Partial since TODO-03 section 24: the matrix store legs (corrupt, repeat, repaired) reuse one OVMF variable store across boots through `SMOKE_STORE_RECORD`, so a second-boot leg can follow that shape.
- [ ] Syscall arg counts are not in the ABI fingerprint
  - `render()` in `scripts/gen-user-abi.py` fingerprints names and numbers. `SYS_EXEC` is slated to go from `(path, len)` to `(path, argv, envp)`, which would land with the fingerprint unchanged. Fixing it closes `00-infrastructure/TODO-04` s41.
- [ ] The release flavor is not built at the section boundary
  - A parameter used only under `KERNEL_TESTS` breaks `KERNEL_TESTS=off` with `-Werror`, and only CI's release job sees it. A release-flavor build in the J1 chain is about 20 s.
- [ ] Does a single session hold up for days, and how much does compaction lose?
  - Untested. Watch the first runs. A WSL restart now resumes a live run by itself (`run.sh autostart`, from the boot service `start` installs); WSL still has to be started for that to happen.
- [x] Do the 36 `awaiting-answer` stamps pick up their answers?
  - They did not: nothing read `todo/answers.md` back. Fixed 2026-11-11: the classifier treats an answered park as open work, 36 stamps and 128 parked items were reopened, and Q17 (leave as is) became a plain Deferred reason.
- [ ] About 445 terminal `Deferred:` stamps are never re-read, and about 60 of them name hardware or an operator decision
  - Done when each has been re-judged under the never-park rule: reopened with an emulated stand-in or a recorded decision, or kept with a `**Needs:**` line. Work a file's stamps when the run is in that file.
- [ ] The run transcript grows about 50 MB a day and a restart resumes the same session id
  - `run_launch` falls back to a fresh session only when the transcript file is missing. Done when a failed resume, or a transcript over a set size, starts a fresh session from the cursor. Frozen file: an attended change.
- [ ] Nothing restarts the heartbeat if it dies, and a busy session that ships nothing is never nudged
  - Done when the boot service (or a timer) respawns a dead heartbeat, and the alert keys on sections shipped per day, not on idleness. Frozen files: an attended change.
- [ ] An expired Codex, Claude or `gh` login stalls every section with no distinct signal
  - Done when a daily probe of the three logins raises the critical alert and names which one failed.
- [ ] Six pytest-style files in `scripts/overnight/tests/` run no tests under `run-all.sh`
  - `run-all.sh` runs `python3 <file>`, and a file with no `__main__` block defines its tests and exits 0 (`test_review_pin.py` and five more). Done when `run-all.sh` runs those through pytest, or each file runs its own tests.
- [ ] At about 7 sections a day the 1,649 open sections take about 8 months if the plan stops growing, and it grows
  - Measured 2026-11-11 from `supervisor-state.json`. One push per section with a CI wait adds about 25 minutes a section. Watch sections per day for a week before tuning anything.
- [x] Do the logged gates matter?
  - No. After a watched run that shipped 22 sections the operator saw no sign any was needed, so all 22 and the log/enforce wrapper were deleted on 2026-11-09 (docs/plans/2026-11-07-run-supervisor.md, Stage 7).
- [ ] Does medium effort hold quality?
  - Writer and all agents moved to medium on 2026-11-06. No section has shipped since, so there is no comparison yet.
- [ ] Context growth is still the dominant token use
  - Compaction at 400K is the current lever. Compare tokens per shipped section across a change to `RUN_COMPACT_AT`. Also unsettled: trimming `CLAUDE.md`, and replay task sets before pinning `general-purpose` and `Explore` to Sonnet.

- [ ] `test-tooling.sh` install-hooks lifecycle failed once in a pre-push run: `pre-push status not ENABLED` with the sentinel present
  - 2026-11-08, push of `4cf186a49`: 1/1414 failed, while the sentinel check just before it passed. It passed in isolation and on the immediate retry; cause not found. If it recurs, capture `install-hooks.sh --status` output and the sentinel path inside the test.
- [ ] CI goes red whenever the runner cannot reach the Ubuntu mirror, and no project code has run
  - 2026-11-11 from 14:22 UTC: nine Build runs in a row failed in `Use GNU coreutils` with `Temporary failure resolving 'azure.archive.ubuntu.com'`, a re-run included; every job installs its toolchain with apt on each run. Fix by running the jobs in the pinned CI image (`CI_CONTAINER=1` already builds in it) so no job needs the mirror.
- [ ] The first section reviewed as passes of the reviewer pin has not been watched end to end
  - The skills switched on 2026-11-11 (`6b61f0726`). Settled when one section goes plan, wave, stamp in the run with no gate it cannot satisfy; fix what it trips on in `.claude/skills/three-pass-review.md` or the gate.

## Baselines

- Queue on 2026-11-07: 208 files need work, 17 are done, 2 are blocked.
- Refused calls, last headless run: 27 of 1,560 (1.7%), with `receiving-review-required` 10 and `build-offload` 6.
- Review depth across 831 sections: dispatches p50 5, p90 13, p99 32, max 62.
- `Build` runs: 37 success, 3 failure and 0 cancelled, of the 40 before the job split.
- Kernel image headroom: 66,500 bytes on the tightest section, down about 85 KB during the 2026-11-07 run.

- 2026-11-11: an attended session in the primary tree rebuilt `build/` twice during the run's smoke matrix (10:24:37, 10:28:24), failing 8 and 11 legs on the pristine-image receipt check; the legs refused correctly and a quiet re-run passed 20/20. `attended_repair_guard.py` covers whole-tree git verbs but not builds; a build from a second session in the primary tree should be steered to its worktree the same way.
- 2026-11-11: `section_commit_gate.py` charged the TODO-10 section 19 C diff to `00-infrastructure/TODO-03`, whose only staged change was a regenerated XREF line number, and refused on that TODO's stale re-adversarial receipt; a content-valid re-adversarial of the same staged diff under TODO-10#19 did not satisfy it, so the commit (25ba272bd, now 6cfbf20e7) used the logged `SKIP_REVIEW_HOOK` opt-out. A staged TODO whose diff is only generated line numbers should not own the C diff.
