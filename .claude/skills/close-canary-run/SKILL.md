---
name: close-canary-run
description: Close out a finished overnight canary -- triage EVERY item the run filed in the newest token-saver + overnight-runner-improvements versions (in batches, so a 99-item month-run file is fully triaged rather than partly carried), verify each claim against the current tree before implementing it, reject with `- [-]` and a reason where a fix is not worth its blast radius, record a verdict for every item, then close those versions and open the next. Use after any canary or overnight run stops (deadline, disarm, breaker, fixpoint).
---

# Close a canary run

The ritual that turns a finished run's observations into shipped fixes and hands
the next run a clean capture surface. Performed by hand twice (v06 -> v07 on
2026-07-31, v07 -> v08 on 2026-08-02); this skill is that sequence with the
failure modes it exposed written into it.

## Hard preconditions

**The run must be STOPPED.** Verify, do not assume:

```bash
systemctl --user list-timers --all | grep -c overnight   # must be 0
bash scripts/overnight/run-liveness.sh                   # must not be RUNNING
git status --porcelain | wc -l                           # the run's WIP must be settled
```

If timers remain, disarm first: `bash .claude/skills/overnight-sequencer/arm-sequencer.sh --disarm`.
A close-out performed against a live run collides with its section work; that is
the collision class the whole attended-repair doctrine exists to prevent.

**Find the ACTUAL newest versions** -- never assume the number:

```bash
ls -1 todo/token-saver/ todo/overnight-runner-improvements/ | sort
```

## 1. Triage EVERY item before implementing ANY -- findings are evidence, not instructions

**Carrying an item forward is a verdict, not a default.** The failure this step
exists to prevent is a close-out that reads the first few items, implements
them, and carries the rest "for next time" -- the backlog then grows every cycle
and the run's own reporting quietly becomes a write-only surface.

**But nothing is implemented because it is written down, either.** A filed item
is a claim made hours or days ago by a session that has since ended, about a
system that has since changed. Verify it against the tree AS IT IS NOW:

- **Does it still reproduce?** Run the probe. A fix may have landed since, the
  code may have moved, or the diagnosis may name a mechanism that was never the
  cause. Of the findings closed in the v07 -> v08 cycle, THREE did not
  reproduce; in the v12 cycle one named a case that had existed since the file
  was written and the real defect was one layer below it.
- **Is it a real improvement, or a preference?** "This would be cleaner" is not
  a finding. Name the concrete cost the current behaviour imposes -- calls
  wasted, a gate misfiring, a false verdict shipped.
- **What does it touch, and what did we spend days tuning there?** A control
  plane that has been converging is worth more than any single item's marginal
  gain. An item that would re-open a settled question is REJECTED even when its
  claim is true.

Classify each into exactly one of four, and write the reason for every one:

| verdict | means | marker |
|---|---|---|
| **IMPLEMENT** | reproduces, real improvement, blast radius understood | stays `- [ ]` until it ships, then `- [x]` |
| **NOT REPRODUCED** | the claim does not hold against the current tree | `- [ ]` + counter-evidence with counts |
| **REJECTED** | true but should NOT be done -- see below | **`- [-]`** + the reason, inline |
| **CARRIED** | real and wanted, but needs a decision, an owner, or a design pass this stop cannot supply | `- [ ]` + what would settle it |

**`- [-]` is the rejected marker, and it is for CAPTURE FILES ONLY.** Verified
2026-08-10: no parser in the repo recognises `[-]` and none errors on it, so an
item marked that way is inert everywhere -- correct for a decided-not-to-do,
WRONG for a real TODO, where invisible means unreachable. Never use it outside
`todo/overnight-runner-improvements/` and `todo/token-saver/`.

Legitimate grounds for REJECTED, each of which has occurred:

- the fix would re-open something recently stabilised, and the item's gain does
  not justify the risk;
- it is refinement below the depth the component warrants (ask the same question
  a review-spawned section must answer: what does a USER hit if this is not
  done?);
- it has been superseded -- a later change made the concern moot;
- the mechanical form was measured and rejected on precision (the "mutation
  claim must write to production" lint flagged 23 of 35 on a clean corpus);
- two items describe the same defect and one is the better statement of it.

## 1a. Scale: a 99-item file is triaged in BATCHES, never in one pass

A month-scale run can file a hundred items or more, and the failure at that size
is different: not laziness, but a single pass that runs out of context and ends
with the tail unread and silently carried.

- **Inventory first, deterministically.** Count the open items in both files
  before reading any. That number is the denominator every later claim is
  measured against, and it belongs in the hand-off.
- **Batch by SURFACE, not by file order.** Items against the same hook, script
  or gate are triaged together: they share the reproduction, they often collapse
  into one fix, and duplicates only become visible side by side. Order the
  batches by blast radius, most dangerous first, while attention is freshest.
- **Every batch ends with its verdicts WRITTEN to the file** before the next
  begins. A batch triaged in context and not recorded is a batch that dies with
  the compaction.
- **Keep a running ledger** of triaged-vs-total and state it in the hand-off.
  `triaged 41/99` is a fact; "worked through the backlog" is not.
- **Bounding the IMPLEMENT set is allowed; bounding the TRIAGE set is not.**
  Every item gets a verdict this stop. Implementation may stop early -- for
  time, for risk, because a design decision is missing -- and the remainder is
  CARRIED with the reason. What must never happen is an item that reaches the
  next version with no verdict attached.

## 2. Implement, with the normal bar

Ordinary repo discipline applies -- these are real changes:

- Kernel/boot/SMP code goes through Codex adversarial review. On 2026-08-02 a
  scheduler fix that passed **28 consecutive boots** was rejected on review for
  a genuine memory-ordering hole; repeated green boots cannot prove an ordering
  property, only an argument can.
- Every fix gets a test that FAILS without it. Mutation-test the important ones.
- A control-plane check that would ERROR on pre-existing violations ships as a
  WARNING first, the corpus is repaired, and only then is it promoted. Shipping
  it blocking would wedge the next run's commits.
- **Order by blast radius, and re-verify after each.** The close-out's own
  changes interact: a gate fixed in item 3 can change what item 7 measures.
- **EVERY control-plane change ships a REFUSAL-DIRECTION control** -- a case that
  must still be BLOCKED, not merely a case that now passes. This is the one
  obligation that replaces an external review pass here, and it is not
  ceremony: it covers the axis nothing else can reach.

  A canary run is a real integration test of the control plane -- a gate change
  gets exercised across dozens of commits within hours, which is stronger
  evidence than any reading of a diff. But it exercises the HAPPY PATH at scale
  and structurally cannot exercise the refusal path, which is the entire purpose
  of a gate. Worked example, 2026-08-10: widening the review gate's allowlist to
  see through `(` grouping was exercised by the run passing `( git push ... )`
  hundreds of times, and the run would NEVER have tried `( rm -rf / )`. The
  canary going green proved nothing about whether the widening opened a hole.
  What actually covered it was a fixture asserting that grouping does not
  allowlist a bad program, and that a loop header whose substitution executes
  first stays refused.

  So: name the case that must still be refused, write it, and watch it fail if
  the change is reverted. Seconds, deterministic, and it never hangs.

**Do not bulk-fix a large filed backlog.** Per-item repair needs per-item
judgment; a mass rewrite moves the problem rather than fixing it.

## 3. Record a verdict for EVERY item -- silence is the failure mode

In the closing version, each item ends as one of:

- `- [x]` **RESOLVED** -- what shipped, at which commit, with the evidence.
- `- [ ]` **NOT REPRODUCED** -- the counter-evidence, with counts. Say what was
  tested so the same claim is not re-filed next cycle.
- `- [-]` **REJECTED** -- true, and deliberately NOT being done. The reason goes
  on the item, not in a commit message: which ground it failed on (re-opens
  something recently stabilised / below the depth the component warrants /
  superseded by a later change / measured and rejected on precision / duplicate
  of a better-stated item), and what evidence settled it. A rejection without a
  reason is indistinguishable from an item nobody read.
- `- [ ]` **CARRIED** -- why it is not being done now (needs a design decision,
  needs an operator, too large for this stop), and what would settle it. Carried
  items are the ones that must be argued for, not the default landing place: if
  a version closes with more carried than resolved-plus-rejected, say so
  explicitly in the hand-off, because that is the shape of a backlog turning
  into an archive.

An item that quietly disappears between versions is the exact black-hole shape
this repo forbids elsewhere. So is an item that appears in vNN+1 with no verdict
recorded in vNN -- the successor inherits the ITEM, never the triage debt.

**Count the verdicts and state the split.** `40 filed: 22 resolved, 6 rejected,
5 not reproduced, 7 carried` is auditable; "closed out the backlog" is not. The
four numbers must sum to the inventory taken in step 1a, and that arithmetic is
the cheapest possible check that nothing was skipped.

## 4. Close the old versions, open the next

Both files get a CLOSED banner naming their successor, then create `vNN+1` with:

- **What shipped in this stop, and is therefore under test** -- so the next run
  knows what is new and what to watch break.
- **Carried forward from vNN** -- every unresolved item, with a back-pointer.
- **Standing measurement obligations** -- the questions the next run must
  answer, with the baseline numbers to compare against. Carry the baselines
  forward; a measurement without one is an anecdote.

Then repoint the doctrine:

```bash
# .claude/state/live-gotchas.md -- the ARMED card must name the new vNN
grep -n "FILE such findings" .claude/state/live-gotchas.md
```

`arm-sequencer.sh` REFUSES to arm while the newest capture file is closed, so a
forgotten step 4 is caught at arm time rather than by findings vanishing into a
closed file mid-run.

## 5. Verify, commit, push

```bash
bash scripts/build.sh && tail -1 build/build.log     # === BUILD OK ===
bash scripts/test.sh QUIET=1                          # kernel + user-mode
bash scripts/test-tooling.sh                          # host tooling
bash scripts/overnight/tests/run-all.sh               # runner control plane
bash scripts/lint.sh                                  # 0 errors
bash scripts/audit-hooks.sh                           # MANIFEST vs settings
bash scripts/test-smoke-matrix.sh                     # 4/4 legs
```

Quote the actual numbers (`superpowers:verification-before-completion`). A leg
that fails on ONE configuration is the finding, not noise -- but re-run first:
the `kvm:2cpu` leg has a documented intermittent history, and the tooling suite
has a documented flaky assertion.

Rebuild the graph cache with `--keep-cache` (without it the cache is DELETED and
every cache-reading tool silently degrades to a fallback heuristic).

## 6. Hand off

Lead with the arithmetic from step 3 -- filed, resolved, rejected, not
reproduced, carried, summing to the inventory. Then what shipped and is now
under test, what each rejection was rejected ON, what is carried and what would
settle it, and what the next run must measure. Then the operator decides whether
to re-arm.

If implementation was bounded (time, risk, a missing decision), say where it
stopped and why -- a close-out that implemented 8 of 22 and says so is honest; one
that implies it did all 22 is the reporting failure this whole surface exists to
prevent.

## What this skill must never do

- **Never carry an item by default.** Carrying is a verdict that must be argued
  for, exactly like rejecting. The measured failure is a close-out that triages
  the first few items and lets the tail roll forward untouched -- the backlog
  then grows every cycle while each individual stop looks productive.
- **Never implement an item because it is written down.** It is a claim made by
  a session that has ended, about a tree that has changed. Reproduce it first,
  and reject it if the fix would re-open something that has been converging --
  a true finding is not automatically worth its blast radius.
- **Never use `- [-]` outside the two capture directories.** No parser in the
  repo recognises it, so in a real TODO it makes work invisible rather than
  closed.
- **Never suppress a finding to make a count look better.** The gap-filling is
  the self-improvement loop working; a close-out that quietly drops items
  destroys the only record that the work exists.
- **Never re-create work that already exists** because it was unreachable.
  Unreachable is not absent: reopen it, reference it, or fix its shape.
- **Never tune a number on reasoning alone.** `ROTATE_HINT_TURNS` was adjusted
  or repaired three cycles running and the threshold was innocent every time.
  Measure first, then change.
