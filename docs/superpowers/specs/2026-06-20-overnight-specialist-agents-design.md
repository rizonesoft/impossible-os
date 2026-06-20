# Design: Overnight Specialist Agents (Approach A -- Advisory)

> Status: approved design, pre-implementation. Date: 2026-06-20.
> Scope: introduce read-only specialist subagents that the existing pipeline
> skills delegate analysis to during interactive and overnight (sequencer) runs.
> Non-goal: parallelism / throughput (that is Approach C, a separate project).

## Problem

The overnight sequencer is a single headless Claude process whose safety is
enforced by hooks that read the **main session's transcript**:

- `run_phase_guard.py` polices only the `Skill` tool (and blocks `AskUserQuestion`
  + self-teardown `Bash`). The `Agent`/`Task` tool passes through unpoliced.
- The section-commit gate + review hooks record evidence (the standalone
  `codex-dispatch.sh` call) from the main session's transcript only.
- The phase-machine state (`.claude/state/*`) is single-writer by design.

Goals, ranked by the operator: (1) higher quality via domain specialization,
(2) more throughput, (3) leaner main-loop context. Throughput (2) fights the
safety model and is out of scope here; this design delivers (1) and (3).

## Decision

Build **read-only specialist subagents** under `.claude/agents/*.md` that the
existing pipeline skills delegate analysis to. Agents are **sensors, not
actuators**: they analyze in their own context and return findings as text. The
main session remains the only thing that edits files, dispatches Codex, touches
phase state, and commits.

This is purely additive: a sharper pre-pass *before* the authoritative Codex
review the gate still requires. No hook changes, no doctrine changes, no new
mechanism.

## The advisory contract

Every specialist obeys all four rules:

1. Read-only tools only -- no `Edit`, `Write`, `git commit`.
2. Never dispatches Codex -- Codex stays a main-session standalone call so the
   commit gate's evidence binding holds.
3. Never invokes a controlled Skill -- avoids racing the phase-machine state.
4. Returns structured findings -- `file:line` + severity + one-line claim. The
   main session triages via `superpowers:receiving-code-review` and remains the
   sole authority on what is fixed.

Safe by construction: the dangerous hooks (section-commit gate, review hook,
phase guard) only fire on `Edit`/`Write`/`Skill`/`git commit`. An agent that
does none of those is invisible to them.

## Roster (5 agents)

| Agent | Model | Tools | Feeds |
|---|---|---|---|
| `kernel-explorer` | opus | Read, Grep, Glob | implement-todo-section step 3 |
| `kernel-quality-auditor` | opus | Read, Grep, Glob | review-todo-section step 7 |
| `boot-quality-auditor` | sonnet | Read, Grep, Glob | review-todo-section step 7 |
| `parity-research-analyst` | opus default; sonnet in gap-audit mode | Read, Grep, Glob, WebSearch, WebFetch | gap-audit Phase 2-3 + review steps 9-12 |
| `review-evidence-mapper` | sonnet | Read, Grep, Glob | review-todo-section Phase 1 (pre-Codex evidence map) |

- `kernel-explorer` -- kernel-tuned execution-path tracer + dependency mapper;
  returns focused file list + integration surface (callers, lock order,
  init-phase placement). Domain-aware upgrade over generic `feature-dev:code-explorer`.
- `kernel-quality-auditor` -- walks all 10 `kernel-code-quality` gates +
  `bare-metal-gotchas.md` against the section diff; SMP/locking is the lead
  dimension.
- `boot-quality-auditor` -- walks all 10 `boot-code-quality` gates against boot
  diffs (UEFI error handling, EBS boundary, `boot_info` ABI sync, POST16,
  framebuffer guards).
- `parity-research-analyst` -- serves two seams via an invocation mode:
  *todo-plan mode* (gap-audit Phase 2 web research + Phase 3 inventory/gap
  classification) and *implemented-code mode* (review steps 9-12 parity +
  false-completeness on shipped code). Merged from two earlier candidate agents
  because the web-research core is shared.
- `review-evidence-mapper` -- reads the section diff + surrounding code (callers,
  related subsystems) and returns a `file:line` evidence map for the
  review-todo-section Phase 1 / pre-Codex step. **Constraint: it supplements but
  does NOT replace the main session's >=2 gate reads.** `phase1_evidence_gate.py`
  counts `Read`/`Grep`/`Glob` on `src/`/`include/` in the *main session's*
  transcript (>=2 required before the adversarial Codex dispatch); a subagent's
  reads are in a separate transcript and do not count. The intended division:
  the mapper does the *breadth* (the long tail of evidence reads, on Sonnet); the
  main loop does *targeted depth-verification* of the 2 most important spots,
  which satisfies the gate and verifies the mapper's top claims rather than
  trusting them. Never bypass the gate via `SKIP_PHASE1_BLOCK`.

Each agent's expertise is a file that already exists (`kernel-code-quality`,
`boot-code-quality`, `bare-metal-gotchas.md`), so agents cannot drift from the
project's real standards -- updating the gate doc updates the agent.

**Model tiering by backstop strength (not uniform).** The deciding principle is
**how strong the independent net behind the agent is** -- sonnet only where a
*second* net genuinely catches a miss, opus where the agent is the thin or sole
net. Backstop audit:

- *Strong backstop -> sonnet:*
  - `review-evidence-mapper` -- the main loop verifies the map's top claims with
    its own >=2 gate reads, so a wrong/incomplete map is caught on verification.
  - `boot-quality-auditor` -- triple net: the main loop walks the same 10 gates
    itself (step 7) AND the step-8 Codex quality dispatch runs; the auditor is a
    third redundant pass.
  - `parity-research-analyst` in *gap-audit mode* -- the mandatory Phase 3.5
    `codex-gap-audit` red-teams the inventory specifically for missed features.
- *Thin or sole net -> opus:*
  - `kernel-quality-auditor` -- the sole specialist net on SMP/lock-order/
    bare-metal, the project's least-forgiving domain; a missed bug is the most
    expensive. Input is a small diff, so the opus premium is modest.
  - `kernel-explorer` -- no direct Codex on exploration; a missed file feeds a
    weaker implementation. Mechanical, but it is the input to code, so keep it
    sharp.
  - `parity-research-analyst` in *review mode* (steps 9-12) -- thinner backstop
    than gap mode (main-loop judgment + re-adversarial 13.5, not a dedicated
    red-team).

**Mode-split for `parity-research-analyst`.** Its two seams have different
backstop strength, so its model differs per dispatch via the per-call `model`
override: the `gap-audit-todo` skill dispatches it with `model: sonnet`; the
`review-todo-section` skill uses its opus frontmatter default. One override point
in one skill.

**Token economics.** Handing work to a sonnet subagent saves opus tokens twice:
(1) the analysis runs at sonnet rates, and (2) the verbose intermediate work --
raw fetched web pages, dozens of file reads, the agent's own reasoning -- never
enters the opus main loop's context; only the distilled findings do. The gap-mode
web research (full web pages x >=9 searches) is the single largest token sink and
is exactly the strong-backstop case kept on sonnet, so the conservative tiering
still captures the biggest win while every thin-backstop net stays on opus.

**Effort is not a per-agent knob** on the Agent-tool path (no `effort`
frontmatter field, no `effort` Agent param); agents inherit the session effort.
Per-agent effort exists only inside a `Workflow` script -- a lever reserved for a
future Approach C, not used here.

**Excluded (YAGNI):** desktop/shell/userland auditors -- their `*-code-quality`
skills are still placeholders. Roster is extensible: when those domains go
active, add an auditor mirroring the matured gate skill.

## Why gap-audit is in and validate is out

- **gap-audit IN:** Phase 2 ("Internet Research -- do not skip") is the heaviest
  context sink in the pipeline (raw WebFetch pages), and Phase 3.5 is a
  **mandatory `codex-gap-audit` red-team of the inventory** -- so the analyst's
  output is adversarially verified by Codex before any TODO edit lands. High
  context cost + downstream verification = delegate.
- **validate OUT:** `validate-todo-file` is mostly mechanical (separator checks,
  Inputs `Glob` anchor-check, 200-char OS-Comparison row cap, numbering shape)
  with a **deterministic backstop** (`scripts/todo-graph/build-and-validate.sh`).
  An LLM agent there would be less reliable than the script for near-zero context
  savings. Deterministic check + low cost = do not delegate.

Reusable rule for future agents: **delegate where context cost is high AND
something downstream verifies the agent's work; never delegate a deterministic
check to a probabilistic agent.**

## Wiring points

At every seam: skill dispatches agent -> agent returns findings -> main session
triages via `receiving-code-review` -> main session does all edits/Codex/commits.
Mandatory Codex steps are never touched; agents are pre-passes before Codex.

1. **`implement-todo-section` step 3 (Explore)** [edit existing text] -- when the
   section touches `src/kernel/` or `src/boot/` and the surface is large
   (>20 files / new domain), prefer `Agent(kernel-explorer)`. Generic `Explore`
   stays the fallback.
2. **`review-todo-section` step 7 (Domain code quality gates)** [add pre-pass] --
   dispatch the domain-matched auditor (`src/kernel/` -> `kernel-quality-auditor`,
   `src/boot/` -> `boot-quality-auditor`) to walk gates against the diff. Main
   session triages, still walks gates itself, proceeds to step 8 Codex unchanged.
3. **`review-todo-section` steps 9-12 (parity + completeness)** [add pre-pass] --
   dispatch `parity-research-analyst` (implemented-code mode, opus frontmatter
   default). Main session folds findings into its analysis; Codex steps 5/8/13.5
   untouched.
4. **`gap-audit-todo` Phase 2-3 (research + inventory)** [delegate heavy phase] --
   dispatch `parity-research-analyst` (todo-plan mode, per-call `model: sonnet`
   override) for web research + inventory/classification. Main session then runs
   mandatory Phase 3.5
   `codex-gap-audit` on the inventory and applies edits. Both unchanged.
5. **`review-todo-section` Phase 1 (pre-Codex evidence)** [add breadth pre-pass] --
   dispatch `review-evidence-mapper` to read the section diff + surrounding code
   and return a `file:line` evidence map. The main session then does its
   targeted >=2 `src`/`include` reads to verify the map's top claims (which
   satisfies `phase1_evidence_gate.py`) and writes the Codex prompt from the
   combined picture. The gate's main-session read requirement is NOT bypassed;
   the mapper only removes the long tail of breadth reading from the opus loop.

**Domain gating:** driven by the section/TODO's touched paths. Non-kernel/boot
sections get no auditor -- the seam is a no-op and the pipeline runs as today.

**Phase-guard interaction:** none. Dispatches happen inside controlled skills
that only run in their allowed phases; the `Agent` tool sails through the guard,
inheriting the skill's phase legitimacy.

## Safety enforcement (deterministic, not prompt discipline)

The four-rule contract collapses into one mechanism: the `tools:` allowlist.

- Grant only `Read, Grep, Glob` to the four code agents (`kernel-explorer`,
  `kernel-quality-auditor`, `boot-quality-auditor`, `review-evidence-mapper`);
  add `WebSearch, WebFetch` for `parity-research-analyst`.
- **No `Bash` for anyone.** Bash is the hole that would allow `git commit`,
  `codex-dispatch.sh`, or a `sed`/`tee` edit. None of the five agents need it
  (they read code and return findings; the main session builds/tests/commits).
  Dropping Bash closes every mutation and evidence-desync path in one move.
- No `Edit`/`Write`/`Skill`/`Agent` granted -> cannot mutate, cannot invoke
  controlled skills (no phase-state race), cannot spawn nested agents.

This is defense-by-capability (like a kernel guard page): remove the capability
rather than detect its misuse. Strictly stronger than the transcript-watching
commit gate, and it sidesteps the open question of whether the repo's PreToolUse
hooks fire inside subagent contexts.

**Verification:** add a `scripts/lint.sh` check asserting every
`.claude/agents/*.md` `tools:` field is a subset of
`{Read, Grep, Glob, WebSearch, WebFetch}`. Any future edit adding
`Bash`/`Edit`/`Write` to one of these agents fails lint. The contract becomes a
CI-enforced invariant.

## Testing and rollout

Build order is load-bearing (a skill edit referencing a missing agent breaks the
live run; the reverse is harmless):

1. Land the 5 agent `.md` files + the lint check.
2. Smoke each agent standalone (trivial prompt; confirm it returns findings).
3. Wire seams one at a time, lowest-risk first:
   - Seam 1 (`kernel-explorer` -> implement step 3).
   - Seam 5 (`review-evidence-mapper` -> review Phase 1) -- read-only supplement,
     low risk; prove it before the auditors so the evidence map is in place.
   - Seam 2 (the two auditors -> review step 7).
   - Seam 3 (`parity-research-analyst` -> review steps 9-12).
   - Seam 4 (`parity-research-analyst` -> gap-audit Phase 2-3) -- last, after the
     agent is proven in review mode.

**Per-seam acceptance:** run the affected skill interactively on a real
section/TODO and confirm (a) the agent dispatches, (b) findings are usable,
(c) the main session still completes all mandatory Codex dispatches and the
commit gate still passes. The seam is correct only if the gates are untouched.

**Live-runner coordination:** the overnight sequencer is currently running. Seam
edits are backward-compatible and domain-gated -- a running skill re-reads its
`SKILL.md` on next invocation, so the live run picks them up on the next
section/TODO with no restart. The only unsafe ordering ("skill edited but agent
file missing") is prevented by step 1's ordering. If preferred, land everything
and `--disarm`/re-arm at a clean file boundary. Operator decision at rollout
time, not a design blocker.

**Doc sync:** update `.claude/skills/README.md` + the CLAUDE.md skills section to
note the agent roster, per the repo Doc Sync rule.

## Files touched

- New: 5 `.claude/agents/*.md` (the roster).
- New: 1 `scripts/lint.sh` check (tool-allowlist invariant).
- Edited: 3 `SKILL.md` (implement-todo-section, review-todo-section,
  gap-audit-todo).
- Edited: `.claude/skills/README.md`, `CLAUDE.md` (doc sync).
- No hook changes, no doctrine changes.
