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

- [ ] **Fix the non-hermetic `ai_workflow_*` tooling tests so CI (`build.yml`) goes green.**
  build.yml has been red for 30+ runs (since `548f59a1b`, 2026-07-02); the runner
  keeps pushing on top of red CI and the failure emails never stop. 5 tests
  (`ai_workflow_stamp` x2, `ai_workflow_gates` x2, `ai_workflow_obligations` large-ledger)
  pass in the dev working repo but fail in a clean `actions/checkout`. Repro:
  `git clone --no-local . /tmp/cirepo && cd /tmp/cirepo && git checkout <HEAD> && TEST_TOOLING_SKIP_LSP_MCP=1 bash scripts/test-tooling.sh`.
  Also fix the fragile `git rm --cached docs/staged-leak.md` cleanup in the
  Check 16 `lint_secret_guard` fixture (errors on a divergent index; secondary
  noise). See memory `project_ci_build_yml_red_since_548f59a1`.

- [ ] **Make the runner CI-aware (self-heal red CI instead of advancing past it).**
  In PREFLIGHT, after the local build/test baseline, dispatch
  `Agent(subagent_type="gh-query-runner", ...)` to check `build.yml` +
  `todo-graph.yml` on HEAD. If a required workflow FAILED on a commit that is an
  ancestor of HEAD (i.e. our pushed work), treat it like the existing
  "HEAD broken at preflight" path: diagnose + fix + commit + push + re-check
  BEFORE new section work. Fail-safe: if `gh` is unauthenticated, emit a NOTE
  and continue (do not block the whole run). `gh` IS authenticated headless
  (rizonesoft token in `~/.config/gh`, scopes incl. `workflow`). Handle
  in-progress runs (skip, not fail) and only act on failures attributable to our
  commits. Wire into `.claude/skills/overnight-sequencer/SKILL.md` PREFLIGHT.

- [ ] **Make the driver/runner model configurable at launch, DEFAULT to Opus (not Fable 5).**
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

- [ ] **Make the fix/review LOOP agent-aware (keep the Codex loops; cut the Claude-side overhead per round).**
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

## P2 -- efficiency / robustness

- [ ] **Harden `test_perf_syscall` against the TCG timing flake under concurrent Codex load.**
  The runner hit `test_perf_syscall.exe: FAIL` twice in the first section, each
  time correctly diagnosed as TCG timing noise (not a regression) and reran green
  -- but that is wasted cycles every section. Same flake class already fixed in
  the `four_dispatch_gate D lock-timeout` test (fixed sleep -> deterministic
  handshake, commit 776ef06c). Give the perf test a load-tolerant bound or a
  deterministic gate instead of a wall-clock threshold.

- [ ] **Verify the headless model-fallback behavior on the first real overload/flag.**
  `--fallback-model opus` is wired into the launch (confirmed live in the process
  args) but was never exercised (Fable 5 handled the whole first section, no
  overload, no safeguard flag). Open questions to confirm on the first real
  event: (a) does `--fallback-model` return to the primary each turn as
  documented? (b) does the interactive `switchModelsOnFlag` safeguard->Opus
  auto-switch ALSO fire under `-p`, or does a Fable-flagged section just DEFER?
  If it defers, decide whether a periodic explicit `--model opus` pass over the
  deferred set is worth adding.

## P3 -- operator UX

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
