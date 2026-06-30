# Claude-Master / Codex-Delegate+Failover -- Design Spec

> Status: DRAFT plan (2026-06-30). Supersedes the "two co-equal drivers /
> Codex-exclusive development" direction in the live-driver and rollout sections
> of [TODO-10 AI Driver Interchangeability](../../todo/00-infrastructure/TODO-10-ai-driver-interchangeability.md).
> **TODO-10 is intentionally NOT edited yet** -- a live Codex overnight run is
> currently validating the existing separate-driver design; this re-scope is
> held until that run is reviewed.
>
> Bare "section N" references below point at TODO-10 (linked above).

## 1. Decision record

Operator decisions that pin this design (2026-06-30):

| Fork | Decision |
| --- | --- |
| Driver model | Claude is the permanent **master** driver. Codex is a **subordinate** that (a) executes delegated phases while Claude is alive and (b) resumes work when Claude is unavailable. No co-equal "Codex-exclusive" mode. |
| Cost model | Claude is **subscription + usage-limits**. The constraint is the limit window, not $/token. Savings = more sections per window + reclaimed cooldown idle. |
| Effort target | **1:4 Claude:Codex (20% / 80%)**. Claude does ~1/5 of the work (judgment only); Codex does ~4/5 (bulk reading, typing, build/test, fixes, stamps, commits, review). |
| Delegation (Claude alive) | **Phase-split.** Claude: design + the irreducible judgment. Codex: the mechanical tail and bulk work. |
| Failover trigger | **Usage-limit + crash/hang.** No manual-handoff command in scope. |
| Codex authority | **Tiered by risk.** docs/host-tooling: Codex ships fully through the gates. kernel/boot: Codex prepares but HOLDS the commit for Claude to bless on return. |
| Fix classification | **Claude classifies (Fix/Reject/Accept), Codex applies.** Implemented as ratify-not-re-derive (see below). |
| Exploration | **Offloaded** off the master Claude. Because Claude subagents count against the Claude limit, exploration runs as **Codex**, not Claude `Explore` agents. |
| Namespace | All logs/files/governance stay in the **shared/Claude namespace**. The separate `.codex/overnight/` tree is retired (single active driver => no concurrent-write collision => no separate tree needed). |
| Reviewer independence | Unchanged and still enforced. Codex-as-driver and Codex-as-reviewer must be distinct runs (`review_run_id != driver_run_id`). |

## 2. The 1:4 lever (what actually saves the window)

Under subscription, the limit window -- not dollars -- is the scarce resource.
Two consequences drive the whole design:

- **Every Claude-model token counts, including subagents.** An `Explore` or
  `kernel-explorer` dispatch is a Claude call against the window. To hit 1:4,
  the bulk reading/exploration must be a **Codex** process that returns a
  distilled brief, not a Claude subagent.
- **Claude must ratify, not re-derive.** Claude's reserved 1/5 is design
  decisions, fix classification, and kernel blessings. If Claude re-analyzes
  from scratch each time, it cannot stay at 1/5. So Codex always **proposes**
  (recommended design approach in the exploration brief; recommended
  Fix/Reject/Accept per finding with file:line evidence) and Claude's step
  collapses to a low-token **ratify / override** decision.

### Work partition

Claude (the reserved 1 -- judgment only):
- Section approach selection, ratifying Codex's exploration brief.
- Fix classification: ratify Codex's per-finding Fix/Reject/Accept recommendation.
- kernel/boot commit blessing on failover return.
- Go/no-go on risky-tier commits.
- Next-step orchestration via the obligation resolver.

Codex (the 4 -- bulk + mechanical):
- Exploration + code reading -> distilled brief (recommended approach included).
- Implementation typing from Claude's ratified design.
- Build, run tests, digest logs.
- Apply fixes Claude classified as Fix.
- Fresh-context review runs (adversarial / consistency / perf), distinct run IDs.
- `stamp.py` generation and gate-checked commits (per tier).

### Effort meter

Add a per-run meter that records Claude vs Codex step counts (and tokens where
available) and reports the realized ratio. Without measurement we cannot tell if
we are actually at 1:4 or quietly drifting back to Claude-heavy.

## 3. Failover contract

### Trigger detection
- **Usage limit:** reuse the existing launcher usage-limit detection (today's
  snooze point) as the failover signal. Instead of idling through cooldown, hand
  the lease to Codex.
- **Crash / hang:** the overnight supervisor watchdog (no-progress limit + step
  timeout) fires failover.
- **Manual:** out of scope (not selected).

### Resume contract
- **Granularity = last completed obligation** (a clean checkpoint), resolved via
  `obligations.py`. Never mid-keystroke.
- **In-flight partial edits:** if Claude dies mid-phase, revert the working tree
  to the last clean checkpoint (last commit or last ledger-recorded clean state)
  before Codex resumes the in-progress obligation. Codex never inherits a
  half-written file of unknown intent.
- **Intent handoff:** Claude writes a durable section-plan artifact at section
  start (reuse the dry-run adapter `plan.md` shape) into the shared namespace, so
  Codex resumes with *why*, not just *what is left*.

### Tiered authority on resume
- docs / host-tooling: Codex ships fully (lease -> obligations -> fresh review ->
  stamp -> gated commit).
- kernel / boot: Codex does everything up to commit, then **holds** -- writes a
  `ready-for-bless` marker. Claude commits on return after a cheap review.

### Hand-back
When Claude returns (limit reset), it reacquires the lease, reads the ledger +
plan, and continues. Sections Codex already shipped just advance the cursor.
Lease transfer must be clean -- never both editing at once.

## 4. Lease semantics (simplified)

With a single active driver at all times, the driver lease is no longer "prevent
two co-equal racers." It becomes "mark current owner + enable clean failover
handoff":
- Records `driver_backend` (claude|codex), `driver_run_id`, and a
  `handoff_reason` on transfer (`limit` | `crash` | `hang` | `delegate` |
  `complete`).
- Failover force-release is automatic but audited via `handoff_reason`.

## 5. Namespace consolidation

- Retire `.codex/overnight/` reports/metrics. Use the shared overnight tree
  regardless of executing backend.
- Keep `.ai-workflow/` as shared derived state (ledger, lease, runs/plans) --
  already backend-neutral.
- Tag every report line and ledger event with `backend=claude|codex` so
  provenance survives inside the single namespace.
- This is a net **deletion** of the current rollout-section complexity.

## 6. Reviewer independence (unchanged)

Even when Codex does the typing and the reviewing, the review obligation is
satisfied only by a fresh-context Codex run with a run ID distinct from the
driver run. The reviewer-normalization item "derive trusted run IDs from the
lease / wrapper / receipt metadata (not prompt text)" becomes **load-bearing**
here, because Codex is now both driver and reviewer far more often.

## 7. Re-scope delta against TODO-10

Keep unchanged (all still required): the inventory, the driver lease (semantics
simplified, not removed), the evidence ledger, the obligation resolver, the
stamp writer, the shared gate library, and the reviewer-role + trusted-run-ID
normalization.

Re-scope:
- **Dry-run adapter (section 8):** keep; reframe `plan.md` as the *intent handoff
  artifact* used by both delegation and failover. Mostly already there.
- **Live adapter (section 9):** reframe from "co-equal Codex-exclusive driver" to
  "Codex delegate + failover executor under Claude master." Add: failover trigger
  wiring, revert-to-checkpoint on resume, tiered hold/bless, clean hand-back.
- **Migration/docs/doctrine (section 10):** doctrine target changes from
  "Codex-exclusive development supported" to "Claude master; Codex
  delegate/failover subordinate" -- a *smaller* doctrinal change that preserves
  the existing authority hierarchy. Retire the `.codex/` namespace here.
- **Regression/pilot (section 11):** replace "Codex-exclusive live mode" pilots
  with: a delegation pilot, a failover-resume pilot (Claude ships obligations
  1-3, is killed, Codex resumes 4-N from the ledger), and a tiered-hold pilot
  (Codex preps a kernel section, holds, Claude blesses).

New work to add:
- Effort-ratio meter (track/report 1:4 Claude:Codex).
- Codex exploration-brief producer (replaces the Claude explorer subagent so the
  Claude window is actually saved).
- Failover trigger + resume contract (own section or folded into the live adapter).

## 8. Risks / things that can break 1:4 or safety

1. **Ratio creep.** If Claude re-derives instead of ratifying, or exploration
   runs as Claude subagents, 1:4 is impossible. Mitigation: Codex-proposes /
   Claude-ratifies pattern + Codex-only exploration + the effort meter.
2. **Unattended kernel failover** is the highest-risk path -- mitigated by
   tiered-hold (kernel commits wait for Claude bless).
3. **Reviewer independence under heavy Codex use** -- the trusted-run-ID work
   must land before live failover ships kernel/host code.
4. **Hand-back race** -- Claude returning mid-Codex-section needs an atomic lease
   transfer so the two never edit concurrently.
5. **Live-run collision** -- TODO-10 edits held until the current overnight run
   is reviewed (operator decision).

## 9. Sequencing (after the live run is reviewed)

1. Review live-run results; fold lessons in.
2. Re-scope the live-driver and rollout sections + Goal/Outcome (show the diff first).
3. Implement in dependency order: finish lease handoff semantics -> stamp writer
   -> shared gates -> trusted run IDs -> exploration-brief producer -> failover
   trigger + resume -> tiered hold/bless -> namespace consolidation -> effort
   meter -> pilots.
4. Pilot order: delegation on a docs section -> failover-resume on a docs section
   -> tiered-hold on a host-tooling section -> only then kernel.
