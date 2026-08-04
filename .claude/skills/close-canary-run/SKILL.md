---
name: close-canary-run
description: Close out a finished overnight canary -- triage and implement what the run filed in the newest token-saver + overnight-runner-improvements versions, record a verdict for every item, then close those versions and open the next ones so the next arm files into a live surface. Use after any canary or overnight run stops (deadline, disarm, breaker, fixpoint).
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

## 1. Triage before implementing -- findings are evidence, not instructions

Read every open `- [ ]` in both files. For each one, **verify the claim at
file:line before writing any fix.** This is the step that pays for itself:

- Of the findings closed in the v07 -> v08 cycle, **three did not reproduce**.
  One named the wrong primary cause (dispatch attribution: the compound-command
  claim was false; the real defect was a section-number regex requiring
  whitespace). One was a re-filing of a diagnosis the tree already recorded as
  wrong (the "Grep is unavailable" claim -- 4,691 successful subagent Grep calls
  against 0 unavailability reports). One was a real symptom with a wrong
  mechanism (build-offload "matches anywhere": the hook segments properly; 3 of
  its 4 cited blocks were genuine bare builds).
- A finding that does not reproduce is **not** a failure of the run. It is the
  system working: the run recorded what it saw and a verifier corrected the
  diagnosis. Record the verdict with the counter-evidence.

Classify each item as **implement now** / **needs a design decision** /
**carry forward**, and write the reason. "Carry forward" with no reason is how a
backlog becomes invisible.

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

**Do not bulk-fix a large filed backlog.** Per-item repair needs per-item
judgment; a mass rewrite moves the problem rather than fixing it.

## 3. Record a verdict for EVERY item -- silence is the failure mode

In the closing version, each item ends as one of:

- `- [x]` **RESOLVED** -- what shipped, at which commit, with the evidence.
- `- [ ]` **NOT REPRODUCED** -- the counter-evidence, with counts. Say what was
  tested so the same claim is not re-filed next cycle.
- `- [ ]` **CARRIED** -- why it is not being done now (needs a design decision,
  needs an operator, too large for this stop), and what would settle it.

An item that quietly disappears between versions is the exact black-hole shape
this repo forbids elsewhere.

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

State plainly: what shipped, what did not reproduce, what is carried and why,
and what the next run must measure. Then the operator decides whether to re-arm.

## What this skill must never do

- **Never suppress a finding to make a count look better.** The gap-filling is
  the self-improvement loop working; a close-out that quietly drops items
  destroys the only record that the work exists.
- **Never re-create work that already exists** because it was unreachable.
  Unreachable is not absent: reopen it, reference it, or fix its shape.
- **Never tune a number on reasoning alone.** `ROTATE_HINT_TURNS` was adjusted
  or repaired three cycles running and the threshold was innocent every time.
  Measure first, then change.
