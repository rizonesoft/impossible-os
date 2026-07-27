# Fix-loop discipline -- why each gate exists

Read on demand from `review-todo-section` step 6. The step text in SKILL.md carries the operative rules and the exact
commands; this file carries the incidents that produced them and the mechanism detail.

Every gate here exists because a review loop spiralled. The pattern is always the same: a fix introduces a new bug the
next round catches, or a round re-derives a verdict whose inputs never moved.

## Pre-dispatch self-diff gate -- the section-28 marathon

TODO-12 section 28 became a **15-dispatch / 67-minute marathon** because each round's fix introduced a NEW bug the next
round caught. The gate's four checks are exactly the shapes that happened there:

1. **Scope** -- the diff addresses ONLY this round's findings. Incidental edits need their own review, and they arrive
   as next round's findings.
2. **Ordering** -- no operation moved before its precondition. In section 28 an IOSB write was reordered ahead of the
   user-buffer probe. Every "reorder for X" fix must not strand a check upstream of the value it guards.
3. **Predicate shape** -- exact-equality where a bit-flag/mask test is required (`x == FLAG` vs `x & FLAG`); sentinel
   and boundary values (`INVALID_HANDLE_VALUE`, -1, 0, cap edges) handled exactly as the original path did.
4. **Regression** -- this fix must not re-open any PRIOR finding. The fix for finding N cannot resurrect finding N-1.

The gate is cheap; a full multi-reviewer Codex round is minutes of wall-clock plus a poll cluster. Only re-dispatch
when the self-diff is clean. A localized fix that passes the gate and is not structural: self-verify and proceed
WITHOUT a full round.

## Test-only / cosmetic deltas -- the section-11 spiral

**Root cause this prevents: 7 Codex reviews for a one-`#define` feature.** When hardening a test in response to a
finding, a FRAGILE allocator- or layout-dependent bound was introduced -- a magic threshold like `delta <= 16` tied to
heap-block-header / `MIN_BLOCK_SIZE` internals. That draws a legitimate NEW finding every round, and spirals.

Use a STRUCTURAL invariant instead: a set-then-delete prewarm asserting net-zero retained growth, an exact
expected-count check, or the existing `test_harness.c` bounded-delta pattern. Those are robust to allocator layout, so
they converge in one round.

Scope of the lighter path (B2, Canary #2 2026-07-14): a fix diff CONFINED to test files (`test_*.c`, anything under a
`/test/` or `/tests/` dir) or to cosmetic edits (comments, whitespace, table alignment, stamp/Notes prose,
numeric-section-ref rewrites) is localized-and-non-structural BY DEFINITION -- it does not change shipped behavior.
Dispatch at most ONE scoped confirming re-adversarial, and ONLY when the test delta changes WHAT behavior is asserted
(a genuinely new or changed assertion, not a moved / renamed / reformatted one). Never a full adversarial + consistency
+ perf trio, and never a fresh multi-round cycle, for a test tweak.

## Convergence gate (P2.1 / P2.2) -- per-kind fingerprint scope

A review kind's verdict is a function of the CONTENT of the files that kind reviews; re-running it over UNCHANGED
relevant inputs only re-derives the same verdict.

Per-kind scope: adversarial / perf / re-adversarial fingerprint **SOURCE only**; consistency / design fingerprint
**SOURCE + TODO**. So a docs/TODO-only fix converges the source-only kinds (skip) and re-runs only the TODO-aware
ones -- one changed file never re-triggers ALL kinds.

Fail-open by design: an unknown kind or a git error returns redispatch, so the gate can only skip a redundant review,
never suppress a needed one. This is the PRIMARY churn mechanism; the round counter is the complementary stall
backstop.

## Round counter -- stall detection, not a round cap

Exit 2 = CAPPED (K consecutive no-new rounds, or the 30-round infinite-loop ceiling). This is STALL detection, NOT a
fixed cap: a review that keeps finding new Critical/High runs as long as it stays productive. On CAP, stop the loop,
spin any unresolved findings to a concrete follow-up `[ ]` + XREF, and escalate.

## Standing evidence map (P2.3) -- why rounds >= 4 must use the mapper

Measured waste this replaces: **23 of 24 rounds re-derived file:line facts inline** (`task.c` re-read ~31x) in the
2026-07-12 run.

The content-addressed agent cache (`agent_result_cache`) keys `review-evidence-mapper` on its SCOPED evidence set
(todo path + section + file paths, line numbers stripped) rather than the volatile round prompt, so a mapper dispatch
over UNCHANGED src/todo content is served from cache for free across rounds. That fixed the original P2.3 volatility
(22 stores / 0 hits).
