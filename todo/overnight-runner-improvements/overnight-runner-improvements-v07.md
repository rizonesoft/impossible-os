# Overnight Runner Improvements v07 -- 24h Canary Findings (armed 2026-08-01)

Findings from the 24-hour canary armed after the 2026-07-31 repair stop. This file is a CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `overnight-runner-improvements-vNN.md` in this directory, which is this one until an operator opens v08.

**Why findings land here instead of being fixed.** The run is explicitly NOT permitted to modify its own control plane while unattended -- `.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json` -- nor the RECEIPT SURFACE (`Makefile*`, `scripts/build.sh`, the ABI generator), which `receipt_surface_guard.py` enforces: build/verification machinery whose content the run's own receipts are computed over must not drift while nobody is watching, because a subtly wrong `build.sh` keeps producing green receipts that no longer mean what they say. Ordinary work is unaffected: kernel code, tests, docs and TODO files are fixed in place as normal.

**Self-improvement is the point of this file, not a side effect.** When the run hits a runner defect -- a gate that misfires, a wedge, a flow inefficiency, an evasion it was tempted into -- the finding is RECORDED HERE in the same turn it is observed, then the run continues.

**What to write.** One item per finding, house style: what was observed live (timestamps, file:line), the mechanism confirmed at source, and what it would cost or risk. A projection is not a finding. If a fix is obvious, describe it -- do not apply it. Lead line <= 250 chars; body as sub-bullets, none over 1,000 chars.

**Scope:** runner flow, gates, wedges, correctness of the run machinery. Cost/token findings go to the newest [`token-saver-vNN.md`](../token-saver/token-saver-v07.md) instead.

**This run is a 24-HOUR CANARY with pre-registered acceptance criteria.** It stops itself cleanly at the deadline (the launcher refuses to spawn past it, so the last segment ends at its own rollover) and aborts early on: breaker trip, two consecutive segments with no section shipped, a rotation loop (>1 rotation per section), or an unexplained gate refusal. The criteria it is being judged against are recorded in [`docs/overnight/run-log.md`](../../docs/overnight/run-log.md) and must be judged on evidence, not impression.

**What shipped in the 2026-07-31 repair stop, and is therefore under test on this run:**

- **Rotation, both halves.** `stream-report.py` now surfaces hook advisories from `attachment` events (they arrive as their own event kind, not as a tool_result -- which is why the hint was invisible through two canaries); and the sequencer's firing procedure now CREATES the WIP boundary instead of waiting for one its own workflow never produces. Measured before the fix: hint fired at exactly 90, delivered 4x to the run, 0 `rollover-wip` attempts, every segment.
- **One operator repair worktree** (`../impossible-os-repair`). The run keeps the primary on `main`; `[SEQ-WORKTREE]` blocks the run from creating worktrees; `arm-sequencer.sh` refuses to arm from anywhere but the primary; run state resolves from the primary worktree so guards do not go inert.
- **Boundary-scoped propagation.** `segment-sync.sh` pulls `--ff-only` at segment start (tree provably clean there); the ship push rebases first because `main` can now move underneath the run.
- **A time bound and automatic aborts.** `--hours N` writes a deadline enforced at SPAWN time, so the run stops after its last segment's verified rollover rather than being killed mid-section; two consecutive no-ship segments or a tripped breaker now STOP the run instead of merely backing it off.
- **Gate fixes.** Review stamps attribute to the section actually dispatched (and clear rather than keep a stale one); `SKIP_CI_PARITY` requires a stated reason unattended; the SECTION token is documented where the dispatch is actually written; hook state is redirectable so control-plane tests stop reading live runner state.

**Carried forward from v06 (still open):**

- **v06 #1 -- control-plane tests flap between runs of the same tree.** Mechanism CONFIRMED by experiment (hooks resolve root from `__file__`/cwd). PARTIALLY addressed: `rotate_hint` and `phase1_evidence_gate` now honour `CLAUDE_HOOK_STATE_ROOT`. Remaining: every other state-owning hook, and pointing the existing fixtures at it. Watch this run for any assertion that fails once and passes on re-run.
- **v06 #5 -- read-only gates block info-gathering Bash by SHAPE rather than effect.** Hit again during this stop: a pure `grep`/`find` command was refused because it began with a variable assignment. Costs retries, never data.
- **v06 #9 -- the commit gate binds reviews to blob SHAs, so fixing a finding invalidates the leg that raised it.** DECISION RECORD, deliberately not fixed: the binding is load-bearing (a review must never be claimable over code it did not see), and a convergence path DOES exist -- fix everything, re-dispatch, land the commit with no edits after the clean verdict. What is missing is EVIDENCE that this path fails in practice rather than merely being awkward. Fixing it blind would mean weakening the one gate that makes a review verdict mean anything, so the next step is measurement, not code: for each section this run, record how many dispatches per kind were needed and whether any section required an opt-out to converge. If sections routinely need more than one re-dispatch per kind, that is the reproduction this needs; if they do not, the finding is friction rather than a defect.
- **v06 #12 -- fold syscall ARG COUNTS into the ABI fingerprint.** Product work owned by `00-infrastructure/TODO-04` section 41, not runner machinery. Left with its owner deliberately.
- **`run-liveness.sh` misreports ARMED for ~15 minutes after a disarm** (`:152` infers ARMED from "no process but the log grew recently" without checking whether any timer exists). Observed at the 22:55 disarm. Cosmetic, but it is the command an operator uses to answer "is it stopped?".


---

_No findings yet._
