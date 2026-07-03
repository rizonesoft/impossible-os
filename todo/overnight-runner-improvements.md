# Overnight Runner -- Improvement Backlog

> **Deliberately outside the sequencer.** This file is named `overnight-runner-improvements.md`
> (NOT `TODO-NN-*.md` and NOT in a `todo/NN-domain/` dir) on purpose: the overnight
> sequencer only traverses files matching `todo/\d\d-<domain>/TODO-\d+-*.md`
> (`sequencer_triage.is_impl_todo`), and the todo-graph only parses `TODO-*.md`
> (`build.py` `rglob("TODO-*.md")`). This name matches neither, so the runner
> never tries to "complete" its own improvement list and the graph never
> validates it. Keep the name this shape. Companion to the doctrine file
> `todo/TODO-Claude-Overnight-Runner.md`.
>
> This is a plain operator backlog, not a formal impl TODO: no IO table, no
> Verified/Quality stamps, no Codex pipeline. Promote an item to a real
> `todo/NN-domain/TODO-*` section only if it grows into subsystem work.

## P1 -- correctness / stop the bleeding

- [x] **Fix the non-hermetic `ai_workflow_*` tooling tests so CI (`build.yml`) goes green.**
  FIXED 2026-07-03. Root cause (systematic-debugging, hypothesis verified both
  directions): the ai_workflow fixtures recorded build-evidence provenance
  pointing at the REAL repo's untracked `build/build.log` (10 sites: 4 CLI
  `--log-path` records + 6 embedded ledger events), and the obligations source
  check opens `root/log_path` and tails for `=== BUILD OK ===` -- present on a
  dev box from routine builds, absent in a clean actions/checkout. Adding ONLY
  a marker build.log to the failing clone flipped 5 FAIL -> 582/582 PASS
  (single cause). Fix: fixture-owned `$AIWF_TMP/fixture-build.log` (rel_path
  keeps out-of-root absolute paths absolute, so no product change needed).
  Also fixed the Check 16 cleanup: `git rm --cached -f` (the fixture
  deliberately leaves staged != worktree != HEAD, which plain --cached
  refuses -- the "docs/staged-leak.md" error on every CI run). Verified:
  clean clone 582/582, dev 583/583.

- [x] **Make the runner CI-aware (self-heal red CI instead of advancing past it).**
  DONE 2026-07-03: wired into `.claude/skills/overnight-sequencer/SKILL.md`
  PREFLIGHT as a triage table after the local baseline -- attributable
  failure (failed run's head SHA is an ancestor of HEAD via
  `git merge-base --is-ancestor`) -> gh-query-runner `--log-failed` digest +
  systematic-debugging + fix/commit/push + one `gh run watch` dispatch to
  conclusion before section work (unfixable -> broken-HEAD stop path);
  non-ancestor/infra failure -> NOTE and continue; in_progress -> NOTE and
  continue (never wait at preflight); gh unauthenticated/network -> NOTE
  "CI check unavailable" and continue. Includes the clean-clone hermeticity
  repro pointer from the 2026-07-03 incident. Live-run validation pending
  (next overnight).

- [x] **Make the driver/runner model configurable at launch, DEFAULT to Opus (not Fable 5).**
  DONE 2026-07-03, both launchers: arm-sequencer.sh defaults flipped to
  `--model opus --fallback-model sonnet` (`--model inherit` sentinel restores
  saved-default; interactive /model untouched; launcher's direct-invocation
  fallback default aligned to sonnet). Conductor (b877662):
  PrimaryModel/FallbackModel per-project fields (defaults opus/sonnet,
  metachar-validated) -> Arm() flag pass-through + dogfood chain
  (arm-sequencer -> overnight-arm --setenv -> launch FALLBACK_ARGS +
  DRYRUN echo); 486/486 tests + test-launch PASS in both repos. Original
  rationale follows.
  Fable 5 (Mythos-tier, above Opus) is overkill as the runner's PRIMARY: Codex
  adversarial/consistency/perf reviews already provide the quality net, and Opus
  is the judgment floor the doctrine reserves for `implement-todo-section` /
  `review-todo-section` / receiving-review (Fix/Reject/Accept) work. Opus is
  cheaper than Fable/Mythos-tier AND sidesteps the safeguard-flag friction on
  kernel/security work entirely (flags route to Opus anyway). Do NOT drop the
  MAIN loop to Sonnet -- Sonnet is for the read-only agents (already there); the
  main loop's receive/defer/debug judgment stays Opus-class. Plan:
  - **arm-sequencer.sh:** already has `--model` / `--fallback-model` + the
    systemd model drop-in (commit 053d1643). CHANGE the default from "inherit
    the saved default" (currently Fable 5) to **`--model opus --fallback-model sonnet`**
    (Opus primary, Sonnet only on transient overload; `--fallback-model`
    re-tries the primary each turn). Keep the flags so Fable 5 / Sonnet can be
    forced per-arm. Leave the operator's INTERACTIVE `/model` default untouched.
  - **Conductor (external WPF runner):** expose the same primary + fallback model
    selection in its launch UI/config, defaulting to Opus, and pass it straight
    into the `claude -p --model <primary> --fallback-model <fallback>` invocation
    so a Conductor-launched run and an arm-sequencer-launched run share ONE model
    policy. The two launchers must not diverge on model handling.
  - **Compatibility contract:** both launchers converge on the same
    `--model`/`--fallback-model` pass-through; the default is Opus in both;
    overrides (Fable 5 for a hard section, Sonnet for a cheap pass) work in both.

- [x] **Enforce per-review-pass agent dispatches (compliance-matrix follow-up, measured 2026-07-03).**
  DONE 2026-07-03 (all three gates):
  - `review_dispatch_gate.py` (BLOCK, PreToolUse Edit/Write/MultiEdit): a
    todo edit ADDING a Verified/Quality-reviewed stamp requires a live
    review-class Skill invocation for this TODO this session (kills the
    section-25 inline-review shape) AND a review-evidence-mapper dispatch in
    the pass window (window = earliest review-kind ts for the TODO - 2h
    grace, else 6h), plus kernel/boot-quality-auditor when the reviewed ship
    commit (adversarial_head) touched kernel/boot paths. Hash-fills into
    existing stamps exempt; fail-open on state/git errors;
    SKIP_DISPATCH_GATE=1 + >= 12-char reason (skip-logged); 9-case selftest.
  - agent_dispatch_recorder now keeps a bounded per-type dispatch map
    (by_type) so gates ask "was THIS agent dispatched", not "was anything".
  - `xref_dispatch_reminder.py` (WARN, once per pass): 3+ distinct foreign
    TODO-NN in tool inputs during a live implement-todo-section pass with no
    fresh xref-dependency-mapper dispatch. Live-run validation pending
    (next overnight). Original measurements follow.
  The agent-lens sweep of run-20260702-141810.log produced a per-invocation
  compliance matrix: kernel-quality-auditor missing in 10/17 review passes,
  review-evidence-mapper missing in 6/17, parity-research-analyst 0/17 (but
  see the CLAUDE.md proportionality note added 2026-07-03: 1-row table syncs
  are legitimately main-session), xref-dependency-mapper 0 dispatches in 20h
  despite several 3+-XREF sections resolving owners inline (log:24-36,
  2672-2679, 5225-5310, 6532-6548), and TODO-12 §25's ENTIRE review ran
  inline with no Skill(review-todo-section) invocation and no Phase-1 mapper
  (log:5160 self-report) while consuming 8 receiving-code-review passes.
  Remaining enforcement work (the warning layer shipped 2026-07-03; these
  need gates):
  - Stamp-time completion check: before review-todo-section can write its
    Verified/Quality-reviewed stamps for a kernel/boot section, require
    evidence of a review-evidence-mapper AND (kernel/boot) quality-auditor
    dispatch for THIS pass (extend the section-review hook family; the
    recorder already writes last-agent-dispatch.json).
  - Detect the "review pipeline executed inline" shape (stamp written with
    zero review-todo-section Skill invocation this session) and remind/BLOCK
    per the same gate.
  - xref-trigger reminder: implement-todo-section step 2's "3+ cross-TODO
    XREFs -> xref-dependency-mapper" is text-only; count distinct TODO-NN
    greps in the first N calls of a section pass and remind.

- [x] **Make the fix/review LOOP agent-aware (keep the Codex loops; cut the Claude-side overhead per round).**
  DONE 2026-07-03 across ALL four loop surfaces: implement-todo-section step
  15 + review-todo-section step 6 (shipped a7ca3263, see Done entry) and --
  closing sweep -- codex-fix-review step 5 + codex-adversarial-review-section
  step 5 (both previously instructed bare in-context `bash scripts/build.sh`
  per round): loop rebuilds/tests via checks-runner BY DEFAULT, lsp-bridge
  for repeated symbol lookups, evidence-mapper re-dispatch on moved fix
  targets. implement-todo-item's single build stays inline (one-shot, the
  documented exception). Enforcement nets: build_offload_reminder (WARN,
  overnight SECTIONS) + inline_churn_monitor (both modes). No Codex-round
  cap added, per the correctness-over-cost rule. Live-run adoption
  measurement: next overnight (compare checks-runner dispatch count vs the
  0/29 baseline). Original rationale + measurements follow.
  The Codex convergence loops stay UNTOUCHED -- each round produces a distinct
  real fix (verified on TODO-12 §8: sem_signal_n rework -> excess-path ->
  wake-budget -> snapshot-budget). The waste is what the CLAUDE main loop does
  AROUND each round: on §8 it ran 21 `build.sh` + 5 `test.sh` + 76 inline
  `bash grep` all IN-CONTEXT and dispatched `checks-runner` ZERO times. The
  default-on agent wiring covers the one-shot steps (explore, initial review,
  gate walk) but NOT the iterative fix loop -- which is where ~90% of the
  tokens go (measured: ~290 context-heavy ops this section, 6 agent dispatches,
  ~2% offloaded). Two offloads, both keeping the main context lean every round:
  - Route every loop rebuild/test through `Agent(subagent_type="checks-runner")`
    -- returns PASS/FAIL + failure digest + artifact path; the main loop quotes
    `build.log` itself (verification-before-completion). Wire into the
    implement/review FIX LOOP, not just one-shot step 16.
  - Re-dispatch `Agent(subagent_type="review-evidence-mapper")` each fix round
    for the file:line edit targets, and use `lsp-bridge` (definition/references)
    for repeated internal-symbol lookups instead of inline `bash grep`.
  Do NOT add a Codex-round cap -- the loops find real bugs; correctness over
  cost on kernel code.
  Run-wide confirmation (run-20260702-141810.log, 20h, measured 2026-07-03 via
  the new `overnight-log-explorer` agent): checks-runner was dispatched 0 times
  out of 38 Agent dispatches while 57 build.sh + 55 test.sh + 17 lint.sh +
  24 todo-graph-validate runs all went through in-context Bash -- the gap is
  run-wide, not a one-section anomaly.

- [x] **Kill the Monitor double-poll antipattern (biggest measured token leak: ~1,100 wasted turns/run).**
  DONE 2026-07-03: "Wait discipline" section added to overnight-sequencer
  SKILL.md (one wait mechanism per wait; prefer the single absorbing
  foreground Bash wait; hold an armed Monitor silently; 30-60s cadence when a
  manual poll is truly unavoidable; batch multi-kind waits into ONE combined
  condition; unrelated forward work allowed while holding). Full rule also in
  codex-design-review "Wait discipline" (the canonical background-dispatch
  doc) with pointer lines at the three other codex background-wait sites
  (adversarial-review-section, review-todo, fix-review). Live-run validation:
  next overnight (compare Holding/Check-poll counts vs the 531/610 baseline
  via overnight-log-explorer). Original measurements follow.
  From §25 (03:30) to the end of the 2026-07-02 20h run, every background
  Codex/agent wait was covered by a Monitor AND a manual poll loop on top of it:
  531 `Holding ...` narrator turns + 610 `Check ... status` Bash polls at a
  ~10s cadence, against only 40 Monitor arms (heaviest single episode:
  `Check §28 final-v2 adversarial` x43, log lines 5999-6083, ~7.5 min). The
  early sections (§8-§24) did it right: ONE foreground Bash call absorbing the
  whole wait (line 1838: `for i in $(seq 1 90); do grep -q "Turn completed"
  ...; sleep ...done` held a 6-min verdict wait in a single turn; line 497 a
  13-min one, zero extra turns). Fix at doctrine level (overnight-sequencer
  SKILL.md wait guidance + the codex-* skills): once a Monitor is armed, HOLD
  until it fires -- no Bash re-polls, no per-poll narrator turns; where a poll
  is genuinely needed (Monitor timeout, host-load slowdown), use a 30-60s
  cadence, never ~10s. Also batch multi-kind review waits into ONE Monitor
  condition (done correctly at line 4996) instead of serial per-kind poll
  clusters (§28 pattern).

## P2 -- efficiency / robustness

- [x] **Harden `test_perf_syscall` against the TCG timing flake under concurrent Codex load.**
  DONE 2026-07-03. Measured root cause: the gate asserted a fixed 200k-cycle
  ceiling on the MEDIAN of 16, and the idle-host median is already 160,535
  cycles (20% headroom) -- under TCG the guest TSC tracks HOST time, so any
  concurrent load drags the median over. Fix: gate on MIN of 32 samples
  (whichever sample escapes host preemption; a real syscall regression
  inflates every sample incl. the min) + one bounded in-test retry round,
  ceiling re-derived FOR the min estimator from measurements (idle min
  ~150k; loaded min-of-two-rounds observed 212,011 -- host bursts outlast
  in-test retries, so the bound carries the load headroom): 400k = ~2.5x
  idle floor, still trips on a 2x syscall-path regression. Trend keys
  (sys_yield_ns/cycles) keep median semantics; sys_yield_min_cycles added.
  Verified: 2 consecutive green SUITE=exec runs (min 158,734 / median
  176,601 on the final run); the mid-hardening run that FAILED at min
  212,011 vs the old 200k proved the load case live.

- [x] **Verify the headless model-fallback behavior on the first real overload/flag.**
  RESOLVED 2026-07-03 by official docs (code.claude.com model-config) +
  policy change, not by waiting for the event:
  (a) CONFIRMED: the fallback switch "lasts for the current turn only";
  next turn re-tries the primary. Triggers are overload/unavailable/
  non-retryable server errors -- NOT auth/billing/rate-limit/flags.
  (b) ANSWERED: there is NO safeguard auto-switch under `-p` -- "a flagged
  request ends the turn with a refusal" in non-interactive mode
  (documented, hardcoded). So a Fable-primary overnight run could
  refusal-loop on kernel/security sections; the 2026-07-03 Opus-primary
  default eliminates the whole class (flags route TO Opus). The
  "periodic --model opus pass over deferrals" idea is moot.
  (c) primary == fallback is UNDOCUMENTED (chain dedupe implies the
  fallback vanishes -- no degradation path); arm-sequencer.sh now WARNs
  on that degenerate shape at arm time.
  Residual: the first real 529 during an overnight confirms (a)
  empirically -- gotcha line filed; nothing further to build.

- [x] **Slice-read the active TODO and big sources instead of whole-file re-reads.**
  DONE 2026-07-03, two layers:
  - `slice_read_reminder.py` (PostToolUse Read, warning-only): whole-file
    RE-read of a > 32 KB file within 30 min of a prior read -> one reminder
    per path per window that a slice read satisfies the Edit freshness gate.
    First full reads and slice reads never warn; selftest + tooling test.
  - Doctrine at the three re-read hotspots: implement-todo-section step 1
    (one full read per pass = orientation, later reads are slices),
    review-todo-section Phase 1 (verification reads are slices at the
    mapper's file:line targets), overnight-sequencer Read-discipline bullet.
  Measured baseline for the next run's comparison: TODO-12 (1441 lines)
  fully Read 59x, task.c (3216 lines) 40x, back-to-back duplicates within
  one second; the Edit "file has not been read yet" re-gate drove 19 of 62
  tool_use_errors. Original text follows.

- [x] **Pre-empt the top three hook-block churn sources (167 PreToolUse BLOCKs in one run).**
  DONE 2026-07-03, each at its root:
  (a) `todo_item_line_length` (77 blocks, 36% re-blocked): block message now
  shows per-line overage + a count-before-retry recipe + an aim-for-230
  margin target (retries were trimmed by feel and landed still-over); plus
  a TODO-write-discipline bullet in the sequencer skill (count BEFORE the
  first write).
  (b) `agent_dispatch_required` (23 blocks, ~once per section): ROOT CAUSE
  was the fixed 30-min freshness TTL expiring mid-section -- the mandated
  step-3 dispatch had happened but long sections outlived the window. Now
  section-pass-bound: a dispatch newer than the live implement-todo-section
  entry's started_ts stays valid for the whole pass (orphaned entries
  ignored; 30-min TTL kept as the no-pass fallback). 5-case fixture test.
  (c) stale oracle cache (3 recurrences): explicit ordering rule in the
  sequencer skill -- all TODO edits -> rebuild -> commit; the rebuild is
  the LAST action before any todo/-touching commit.

- [x] **Find out why MCP-first never fires in headless runs, then fix the root cause.**
  DIAGNOSED + RESOLVED 2026-07-03 (systematic-debugging, empirical repro).
  ROOT CAUSE: a startup RACE, not a connection failure and not approval.
  Headless `claude -p` discovers the project MCP servers and connects them
  ASYNCHRONOUSLY (~0.7s stdio handshake each -- neither is slow; both verified
  handshaking) but does NOT block session init on the connection, and no knob
  forces it (`MCP_TIMEOUT`/`MCP_TOOL_TIMEOUT` tested: no effect). On a cold
  start -- the launcher's exact shape -- the init tool-list snapshot catches
  both servers still `pending` ~2/3 of the time (measured: 2 of 3 cold runs =
  0 mcp tools; the third = 29 tools + a successful `mcp__todo-graph__stats`
  call), so the session begins with zero `mcp__` tools and cannot call them.
  Approval is NOT the gate (`hasTrustDialogAccepted:true`; empty
  `enabledMcpjsonServers` still loads them). No launcher-side deterministic
  fix exists (init is async by design) and a non-deterministic pre-warm would
  be a band-aid. FIX = the "explicitly scope the doctrine" branch: CLAUDE.md
  MCP Usage rule 3 + overnight-sequencer skill + docs/infrastructure/
  mcp-usage.md now scope MCP-first to "when the tools are loaded this
  session" and point the headless fallback at the DETERMINISTIC CLIs the
  servers wrap (`scripts/todo-graph/query.py`, `resolve_symbol.py`) + the
  Grep/Glob tools (never Bash grep/sed). The Grep-tool-over-Bash-grep floor
  is MCP-independent and stays always-on; NO MCP-non-use enforcement was
  added (can't penalize a race the runner cannot win).

- [ ] **Self-review each fix diff against the prior findings list before re-dispatching Codex.**
  §28's review marathon (15 dispatches, 67 min) was fix-then-regress cycling:
  the original real bug (unprobed user buffer, line 5669) was fixed, but each
  round's fix introduced a NEW regression the next pass caught (iosb-probe
  reorder 5850, equality-vs-bit-flag 5900, INVALID_HANDLE_VALUE 5910) --
  ~5 full multi-reviewer rounds instead of 1-2. A pre-dispatch self-diff pass
  against the round's findings is cheap; a full Codex round is minutes of
  wall-clock plus a poll cluster.

- [ ] **Add `QUIET=1` to the straggler test.sh calls.** 11-15 of the 55 test.sh
  invocations ran without `QUIET=1`, pulling full-suite PASS output inline;
  the other ~42 already do it correctly.

## P3 -- operator UX

- [ ] **Fix the lsp-bridge pyright health gap (found during MCP-race diagnosis 2026-07-03).**
  `scripts/lsp-mcp/bridge.py --warm-start=c,py` fails the Python LSP probe:
  `lsp-binary-missing: 'pyright-langserver' availability probe failed (binary
  missing on PATH OR present but the npm shim's sibling pyright CLI failed
  --version)`. clangd (C) warm-starts fine. Effect: even when lsp-bridge IS
  connected, Python symbol queries error out. Orthogonal to the MCP startup
  race (which is now scoped/resolved). Install/repair `pyright-langserver` on
  the dev+overnight host or make the bridge degrade cleanly (skip py warm-start
  when the binary is absent instead of logging restart-failed). Owner:
  TODO-07 lsp-mcp-bridge.

- [ ] **Fix / document the `latest.log` path friction.**
  `tail -f .claude/overnight/reports/latest.log` fails unless run from the repo
  root (relative symlink) and only after the first launch creates it. Either
  print the ABSOLUTE path in the arm-sequencer summary, or add a tiny
  `overnight-tail` helper. Minor.

## Evaluated and rejected (recorded so we don't re-litigate)

- [x] **`tuannvm/codex-mcp-server`** (evaluated 2026-07-02) -- NOT adopted. An MCP
  server wrapping the same Codex CLI we already drive; exposes `codex` / `review`
  / `websearch` / session tools (522 stars, ISC, active). Rejected because: (1) it
  duplicates existing infra -- `codex-companion.mjs` already yields structured
  verdicts + task-thread sessions, and `web-research-analyst` covers web search;
  (2) the ENTIRE evidence/gate/receipt layer keys on the Bash shape
  `bash scripts/codex-dispatch.sh '[review-kind: ...] ...'` (section-commit gate,
  `codex_review_completed` receipt, four-dispatch gate, receiving-review gate,
  model-flag block, `runner_bash_guard`); an MCP tool call is invisible to all of
  them, so adopting it means rewriting the whole layer and forking the one
  canonical dispatch shape; (3) MCP servers needing interactive `codex login` may
  be unauthenticated in the headless systemd overnight run -- the exact context
  we are improving; (4) its general `codex` coding-assistant tool invites
  Codex-as-implementer, which the TODO-10 tool-neutral re-scope deliberately
  removed (Codex is the sole external reviewer, never a driver). Reconsider only
  if the repo ever needs a reviewer path the CLI genuinely cannot provide.

## Done (recorded so we don't redo)

- [x] Model policy: `--model` primary + `--fallback-model opus` wired into the
  launcher, captured at arm time (commit 053d1643).
- [x] `agent_dispatch_recorder` accepting the harness-renamed `Agent` tool name
  (was keying on the old `Task`); fixed by the runner itself (commit 78a69f8c).
- [x] `overnight-log-explorer` agent created 2026-07-03 (`.claude/agents/`,
  sonnet analyst, Read/Grep/Glob): run-transcript cost/efficiency digests
  (tool-call accounting, wait/poll waste, churn). Source of the measured
  counts in the P1/P2 items above (three parallel lens sweeps over
  run-20260702-141810.log; load-bearing counts re-verified by the main
  session per the trust contract).
- [x] Fix/review LOOP wiring shipped 2026-07-03: implement-todo-section
  step 15 and review-todo-section step 6 now route loop rebuilds/tests
  through checks-runner BY DEFAULT (with the measured 21-build/§8 rationale
  inline) + re-dispatch review-evidence-mapper on moved fix targets +
  lsp-bridge for repeated symbol lookups. Backed by two warning-only hooks:
  `build_offload_reminder.py` (overnight SECTIONS, bare build/test Bash with
  no recent dispatch -> checks-runner reminder; warning-only because
  checks-runner's own Bash traverses the hook) and `inline_churn_monitor.py`
  (25+ inline ops since last dispatch -> route reminder, interactive +
  overnight). Live-run adoption still to be verified on the next overnight.
- [x] review-todo-section step 7 duplicate gate-walk removed 2026-07-03: a
  fresh same-diff kernel/boot-quality-auditor dispatch now IS the mechanical
  gate pass (main session triages findings per receiving-code-review);
  the main-session domain-skill re-read/re-walk is required only when the
  auditor was not dispatched or failed. Measured: 8/8 auditor dispatches in
  run-20260702-141810.log were followed within seconds by a full 235-line
  kernel-code-quality/SKILL.md re-read (log:402/403 through 5694/5696).
