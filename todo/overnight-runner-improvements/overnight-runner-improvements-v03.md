# Overnight Runner v03 -- Improvement Backlog (items filed 2026-07-28 14:00 ->)

Successor to [`overnight-runner-improvements-v02.md`](overnight-runner-improvements-v02.md), which is bounded to items filed on the 02:03 attended canary of 2026-07-28. This file collects findings from the SECOND attended canary of 2026-07-28 (armed 13:59, proving the control-plane changes in `4466c81f..01c0d634`) onward. A CORRECTION to an existing v02 item stays in v02 next to the item it corrects, since moving it would orphan the history.

Same discipline as its predecessors: every item is observed live on a real run, not derived from reading the code, and a projection is not a finding.

---

## Doc bugs that cost real turns

- [ ] **The convergence gate is documented as `bash <a python file>` in three skills; FOURTH recorded recurrence, and the failure mode silently defeats the gate.**
  Observed live 2026-07-28 14:13:58 on the second canary: the run issued `bash .claude/hooks/review_convergence.py should-redispatch 'todo/00-infrastructure/TODO-04-usermode-test-framework.md#20' design`, it failed, and the run re-issued it with `python3` four seconds later, noting "the skill's `bash review_convergence.py` invocation is wrong -- it's a Python file". One wasted turn, and only because the run happened to notice.
  **The silent-failure path is the real cost.** `review_convergence.py` carries `#!/usr/bin/env python3`, so `bash` fails with "syntax error near unexpected token" and **exit 2**. The gate's contract is exit 1 = CONVERGED (skip) and exit 0 = redispatch, so an exit 2 fail-opens to REDISPATCH. A run that does NOT notice the error therefore re-dispatches a full Codex round that the gate would have suppressed, and reports nothing wrong. This is the mechanism `live-gotchas.md` already recorded on 2026-07-17 in exactly those words.
  **It has now been logged four times and never fixed at source:** `live-gotchas.md` entries dated 2026-07-15, 2026-07-17 and 2026-07-25 (the last one covering `review_round_guard.py` too), plus this live occurrence. Two of those entries carry expiry dates (2026-08-17, 2026-09-01) and will silently lapse, taking the only surviving record with them. A gotcha that has recurred four times is not a gotcha, it is an unfixed bug.
  **Where the wrong shape is written** (`bash` on a Python file, both verbs): `review-todo-section/SKILL.md:46`, `codex-fix-review/SKILL.md:83`, and `overnight-sequencer/SKILL.md:157-161`. The same file also documents `bash .claude/hooks/review_round_guard.py`, which has the identical defect.
  **NOT fixed during this canary, deliberately.** `overnight-sequencer/SKILL.md` is FLOW-CRITICAL control plane (verified: `control-plane-match.sh --flow-critical` matches it; the other two skills do not), and patching it mid-run would make the canary stamp cover a skill the run never used -- the same reasoning this repo already applied to `arm-sequencer.sh` in v02. A partial fix of only the two non-flow-critical files would leave the sequencer's own copy wrong, which is incoherent. Land all three together after the stamp, and add a lint check so the shape cannot come back: any `bash <path>.py` in `.claude/` or `scripts/` documentation is a defect, since a `#!/usr/bin/env python3` file is never runnable by bash. That check is what turns this from a fifth gotcha into a closed item.
  XREF: `token-saver/token-saver-v02.md` (item: "DETERMINED + FIXED 2026-07-28: the convergence gate was not mis-keyed") -- that item found the gate suppressed 0 of 4 rounds and fixed the four unscoped review kinds. This is the OTHER half of the same symptom: even a correctly-scoped gate suppresses nothing when the caller invokes it in a way that always exits 2.

---

## Gates that behaved correctly (recorded so they are not re-litigated)

- **`receiving_review_required.py` BLOCKed an Edit at 14:22:19 and the run recovered in 3 seconds.** The run had verified two Codex `design` findings at file:line but had not formally received the review wave, then went to edit `include/kernel/sched/syscall.h`. The hook blocked with `[receiving-review-required] BLOCK -- Codex review at 2026-07-28T12:14:27+00:00`; the run replied "The gate is right -- I verified the findings but never formally received the review wave" and invoked `Skill(superpowers:receiving-code-review)`. Not a wedge, not a finding: the gate caught exactly what it exists to catch and the run took the intended exit unaided. Recorded because a BLOCK in a run log reads like a failure at a glance and this one was the system working.
