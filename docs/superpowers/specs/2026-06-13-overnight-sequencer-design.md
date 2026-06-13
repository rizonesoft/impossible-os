# Overnight Sequencer -- Design Spec

> Status: approved 2026-06-13. Supersedes the project-native `overnight-todo-runner`
> (System A). Plugin = generic scheduler; repo = the deterministic brain.

## North star

Start the runner once and **come back in a month to a repo that is implemented,
tested (KVM/TCG + unit), Codex-reviewed, and committed** -- everything that can be
done autonomously is done. The only thing waiting for the human is a short
**punch-list** of items that *physically* cannot be automated (bare-metal /
native-Windows-WHPX sign-off on the real i5-4210U; rare genuine product
decisions). The runner never stops to ask, never gets stuck, and never ships
broken code.

## Architecture: plugin schedules, repo decides

- **Plugin (rizonetech `overnight-runner`, unchanged):** systemd main + watchdog
  timers, `overnight-launch.sh` headless launcher, usage-limit snooze,
  linger-survival, `flock`. We do NOT fork or edit it for logic.
- **Repo (`impossible-os/.claude/`, new):** the sequence, the hard gates, the
  triage, and the ChromeMCP-off, all version-controlled with the repo, durable
  across plugin updates, invisible to other projects.

The plugin launches a **repo-owned sequencer skill** instead of its generic
`/overnight-runner:start`. The repo provides the brain via the shim's headless
`claude -p` prompt (`/overnight-sequencer`).

## Components

| Component | Type | Role |
|---|---|---|
| `overnight-sequencer` | Skill (`.claude/skills/overnight-sequencer/`) | The deterministic driver: preflight, then per-pass (triage, validate, gap-audit, sections, advance) to fixpoint. |
| `run_phase_guard.py` | Hook (`.claude/hooks/`) | Phase state machine. PreToolUse + Stop. Hard-blocks (`exit 2`) any tool call outside the current phase's allow-list. Owns `.claude/state/sequencer-run.json`. |
| `sequencer_triage.py` | Helper | Queries the todo-graph oracle; classifies each file/section DONE / DONE-UNSTAMPED / NEEDS-WORK; returns the next unit in traversal order. Pure + unit-tested. |
| `arm-sequencer.sh` | Repo wrapper | Wraps the plugin's `overnight-arm.sh` (`--mode bypassPermissions`, `--watchdog "*:0/10"`), and writes the per-unit ChromeMCP-off drop-in. |

## Canonical sequence (the phase machine)

```
RUN START
  PREFLIGHT : todo-graph build; HEAD build + unit tests green?
              broken HEAD -> record top-level blocker, complete cleanly, stop watchdog
PER PASS (repeat until a whole pass makes zero progress = FIXPOINT):
  for domain in 00..13 (numeric):
    for file in domain (numeric):
      TRIAGE (graph oracle):
        DONE            -> skip file
        DONE-UNSTAMPED  -> review-todo-section per unstamped section, then skip
        NEEDS-WORK      -> VALIDATE -> GAP_AUDIT -> SECTIONS
      VALIDATE  : Skill(validate-todo-file)                      [allow-list: only this]
      GAP_AUDIT : Skill(gap-audit-todo) (+ mandatory codex-gap-audit) [only this]
      SECTIONS  : for section in Implementation-Order order:
                    DONE ([x]+Verified)              -> skip
                    DONE-UNSTAMPED ([x], no stamp)   -> Skill(review-todo-section)
                    NEEDS-WORK ([ ] / runnable [/])  -> Skill(implement-todo-section)
                                                        (step 20 -> review-todo-section)
                    blocked-[/] (XREF unmet)         -> defer to ledger, retry next pass
      ADVANCE   : next file -> next domain
FIXPOINT (pass produced no progress) -> auto-disarm timers -> final report + punch-list
```

### Phase-guard rules (the "no deviation" teeth)

- Each phase has an allow-list of skills/tools; everything else triggers `exit 2`
  BLOCK with a message naming the expected next action.
- `AskUserQuestion` is **blocked in every phase** (unattended: decide-or-defer).
- The existing within-section gates (design-review, adversarial, section-commit,
  review-required, skill-step-block) keep running **inside**
  `implement/review-todo-section`. The phase-guard governs the **outer** sequence;
  those govern the **inner** pipeline. Defense in depth.
- State (`.claude/state/sequencer-run.json`): the run **cursor only** --
  `pass_no`, `domain`, `file`, `phase`, `section_idx`, `progress_this_pass`,
  `started_at`. No deferral ledger here: deferrals live in the TODO files
  (`[/]` + Deferred/Accepted stamps) and are queried via todo-graph. A watchdog
  relaunch resumes mid-sequence exactly where it crashed.

## Triage / done-detection (graph oracle)

Source of truth = `build/todo-cache.json` (via `scripts/todo-graph/` + `query.py`).
Per file/section signals:
1. IO rows: any `[ ]`, or any `[/]` **runnable now** (not blocked by an XREF to an
   unfinished section elsewhere)?
2. Stamps: does every `[x]` section carry a `Verified:` stamp?
3. `validate.py` `stub-behind-stamp` / `status-transition` flags.

Classes: **DONE** (all `[x]`+Verified, or only blocked-`[/]`) skip; **DONE-UNSTAMPED**
(`[x]` rows, missing stamps, old-system work) run `review-todo-section` (audit; no
scope injection); **NEEDS-WORK** (`[ ]`/runnable `[/]`) run the full sequence.

## Fixpoint loop

A run sweeps the whole repo repeatedly. Most blockers are **temporal** (section A
blocked on section B in another domain); the runner also does B, so A completes on
a later pass. Deferred items are retried every pass. The run ends only when a
**complete pass changes nothing** (no section newly completed, no deferral cleared).

## Blocker / failure taxonomy

| Situation | Handling | Result |
|---|---|---|
| Would ask / needs human decision | phase-guard blocks AskUserQuestion; take conservative/no-op choice + log, or defer | advance |
| Manual / hardware step | recognized blocker category, defer with note | advance |
| Prerequisite unfinished (XREF unmet) | defer blocked-with-XREF | advance, retry next pass |
| Hard failure (build/test/smoke/Codex/commit-gate) | skills' bounded fix loops; if still failing, roll back (commit nothing), defer w/ diagnostic; retry next pass | advance, **nothing broken shipped** |
| Persistent failure (identical error across K=3 passes) | escalate to residual punch-list with diagnostics | advance, surfaced to human |

**Deferrals use the existing machinery, not a new ledger.** The repo already
owns the full deferred-item / loose-end ecosystem and the runner is only *aware*
of it:
- Durable record = TODO-file `[/]` markers + `Deferred:`/`Accepted:` stamps
  (concrete XREFs), created by `review-todo-section` / `implement-todo-section`.
- Query = todo-graph `deferred` / `deferred-by`.
- Resolution = review/implement inbound-sweep + deferred-item-resolution +
  filed-in-owner checks (a deferral is swept when its target item closes).
- Closure = `complete-todo-file` loose-end sweep at file completion.

**Fixpoint convergence rides those signals:** `sequencer_triage.py` already reads
a stamped `[/]` as DONE-for-now, so once a section is properly deferred by the
normal skills, the oracle recognizes it and the loop converges -- no parallel
ledger. The only NEW human-facing artifact is the per-run report the plugin
already writes (`.claude/overnight/reports/`); deferrals are never silent to the
human because they live in the TODO files + todo-graph.

## Watchdog = pure failover

- Cadence `*:0/10` (every 10 min); `flock` makes a tick a **no-op if a run is alive**.
- Relaunch only when no run alive **and** not cleanly finished (crash / usage-limit / kill).
- On FIXPOINT the sequencer **auto-disarms its own timers** + writes `run-complete`
  sentinel: no more relaunches (fixes the spin-after-done bug).

## ChromeMCP per-project disable

`arm-sequencer.sh` writes `~/.config/systemd/user/overnight-impossible-os.service.d/no-chromemcp.conf`
(and the watchdog unit's) with:
```
[Service]
Environment=MCP_NO_AUTO_CHROME=1
Environment=MCP_NO_AUTO_BRIDGE=1
Environment=OVERNIGHT_NO_CHROMEMCP=1
```
Per-unit env: **only impossible-os** units are affected; other projects' overnight
runs keep ChromeMCP. `MCP_NO_AUTO_CHROME/BRIDGE` stop the Chrome auto-launch with
the current plugin; `OVERNIGHT_NO_CHROMEMCP` is honored by a 1-line guard added to
the launch script's lane block (upstream tweak) for a full lane-skip. The wrapper
re-writes the drop-in on every arm, so it survives re-arm.

## bypassPermissions safety

`arm-sequencer.sh` always passes `--mode bypassPermissions`; the watchdog inherits
the same mode (a weaker mode stalls on relaunch). Safe **because** of the two
enforcement layers: the run executes shell/git freely but physically cannot skip a
phase, ship un-reviewed code, or commit a failing build/test.

## System A retirement

After the new system is verified: delete `.claude/skills/overnight-todo-runner/`,
`.claude/hooks/overnight_guard.py`, its 3 `settings.json` hook entries
(SessionStart/UserPromptSubmit/Stop), and the rows in CLAUDE.md skill table,
`.claude/skills/README.md`, `.claude/hooks/MANIFEST.md`. The new phase-guard hook
replaces overnight_guard's role.

## Build order

1. `sequencer_triage.py` + unit tests (pure; gates everything else).
2. `.claude/state/sequencer-run.json` schema + `run_phase_guard.py` (phase machine)
   + unit tests for allow-lists / transitions / blocked-tool detection.
3. `overnight-sequencer` skill (SKILL.md) encoding the sequence + doctrine.
4. Wire `run_phase_guard.py` into `.claude/settings.json` (PreToolUse + Stop).
5. `arm-sequencer.sh` (wrapper + ChromeMCP drop-in) + upstream `OVERNIGHT_NO_CHROMEMCP` guard.
6. Dry-run mode (`SEQUENCER_DRY_RUN=1`): walk phases on one TODO without commits.
7. Guarded first live run on a single small NEEDS-WORK file; verify ledger + auto-disarm.
8. Retire System A; update docs (CLAUDE.md, READMEs, MANIFEST).

## Testing

- Python helpers (`sequencer_triage`, `run_phase_guard`) get unit tests, run via
  `scripts/test-tooling.sh`.
- Dry-run walk of the phase machine on a fixture TODO (no commits).
- Guarded single-file live run before full-repo arm.

## Rollback

Each component is additive until step 8. If the new system misbehaves, disarm
(`arm-sequencer.sh --disarm`) and System A is still present until step 8 completes,
so overnight capability is never lost mid-transition.

## Residual (the human punch-list)

Irreducible, surfaced at fixpoint: bare-metal / WHPX platform sign-offs on real
hardware, and any persistent failures with captured diagnostics.
