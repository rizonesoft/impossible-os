# .fusion -- OpenRouter Fusion escalation (apex tier)

The last rung of the Claude -> Codex -> Fusion escalation ladder. A panel of cheap,
diverse models (GLM 5.2 + Kimi K2.7-code + DeepSeek V4 Pro) deliberates and a cheap
judge (GLM 5.2) synthesizes, via OpenRouter's `openrouter/fusion`. The main Claude
thread is the real review layer -- it reads + validates Fusion's output before
acting, so we do not pay OpenRouter for a duplicate Opus judge.

## Off by default

Nothing here calls the network unless BOTH:
1. `FUSION_ENABLED=1` is set in the environment, and
2. a key exists at `.fusion/secret` (one line) or `OPENROUTER_API_KEY` is set.

## Setup (opt-in)

```bash
cp .fusion/secret.example .fusion/secret   # then replace the placeholder with your key
export FUSION_ENABLED=1
```

`.fusion/secret.example` is the tracked one-line template; `.fusion/secret` (your
real key) is gitignored.

## Spend safety

- Per-run cap `max_calls` (config.toml) + a live balance floor: each call first
  checks `GET /api/v1/credits` and skips if remaining < `min_credits`.
- Fail-open for the runner (an error never blocks the pipeline), fail-CLOSED for
  spend (an unknown/low balance -> skip, never spend blind).

## Data leaves the trust boundary

Enabling Fusion **transmits the context you send** (failing code, diffs, logs) to
OpenRouter, which fans it out to the external panel providers (Zhipu/GLM,
Moonshot/Kimi, DeepSeek) plus OpenRouter's auto web-search (Google); the GLM judge
is Zhipu, already in the panel. Your kernel source leaves your
machine to those services. Only enable Fusion if that is acceptable for this code.
The caller sends BOUNDED context (`--context-file` is capped at 60 KB), never the
whole tree. In interactive auto-mode, a source-to-external crossing is correctly
blocked until you approve it; the headless overnight run uses bypassPermissions and
is gated instead by `FUSION_ENABLED` + the spend caps.

## Files

- `fusion_escalate.py` -- the caller (`--mode stuck|review`, brief on stdin).
- `config.toml` -- panel / judge / caps.
- `secret` -- your OpenRouter key (GITIGNORED, never commit).
- `dataset.jsonl` -- collected escalation in/out + outcome for evals/distillation
  (GITIGNORED, local only).

## Observed cost / latency (real smoke tests, 2026-06-27)

| Question | Judge | Web-search | Output | Cost | Wall-clock |
| --- | --- | --- | --- | --- | --- |
| Cross-CPU spinlock (acquire-in-ISR/release-in-DPC) | Opus | yes | 53 KB | $0.61 | < 5 min |
| Treiber-stack ABA / TSO (pure reasoning) | GLM | no (prompted off) | 9 KB | $0.052 | ~2 min |
| 8-factor Zen4 heisenbug (below) | GLM | yes | 89 KB | $1.16 | **16 min** |

Takeaways: web-search is the dominant cost AND latency driver (prompting "do not
web-search" on pure-reasoning questions cut a call ~12x); the GLM judge keeps cost
down with no quality loss (the main Claude thread is the real review layer); and a
genuinely brutal question takes **~16 min** -- which is why the runner dispatches
Fusion ASYNC (see the spec's job-queue model) rather than blocking on it.

## Worked example: a brutal multi-factor heisenbug

This is the kind of problem the apex tier exists for -- where a single model flails.

**The problem (8 conditions, ALL required to reproduce; remove any one and it
vanishes):** a from-scratch x86-64 SMP kernel triple-faults only on (1) AMD Zen 4,
(2) with SMT enabled, (3) after 4-9 h uptime, (4) with one specific Phison NVMe as
boot device, (5) with C-state C6 allowed; (6) the faulting RIP is inside the LAPIC
timer ISR at a valid instruction with sane state; (7) adding ANY printk to the ISR
makes it vanish; (8) MTTF ~6 h, not reproducible on demand. Reason from
microarchitecture up; justify each condition; give a diagnostic plan that needs
neither on-demand repro nor ISR output; commit to one root cause + fix.

**What Fusion produced (89 KB):** 5 candidate root causes each tied to named AMD
errata, a likelihood ranking, a "why is this condition load-bearing" justification
for all 8 conditions, a 4-phase instrumented diagnostic plan, and a committed root
cause:

> **Zen 4 microarchitectural front-end corruption during asymmetric CC6 entry with
> SMT enabled** -- the active thread's micro-op cache delivers a wrong instruction
> during the sibling's CC6 power-gating microcode sequence.

The standout insight (the kind a single stuck model misses): the captured timer-ISR
RIP and sane state are **red herrings** -- the first interrupt delivery succeeded;
it is the *nested* exception delivery during the inconsistent CC6-exit window that
fails -> #GP -> double fault -> triple fault. So the bug is not in the timer ISR at
all; it is in nested exception delivery across a power-state transition.

The panel members committed to *different* specific culprits (e.g. one to erratum
#1485, the synthesis to front-end corruption) and the judge reconciled them --
divergence-then-synthesis is exactly why a diverse panel beats asking one model
again.

## Head-to-head: Fusion vs Opus 4.8 alone (same brutal heisenbug)

The identical 8-condition brief was put to a clean single-shot Opus 4.8 (no
conversation context, no web search, same system prompt) as an honest baseline.

| | Fusion (3-panel + GLM judge) | Opus 4.8 alone |
| --- | --- | --- |
| Wall-clock | 16 min | **1.4 min** |
| Cost | $1.16 | **$0.13** |
| Output | 89 KB | 13 KB |
| Committed root cause | Zen 4 front-end micro-op-cache corruption during asymmetric CC6 entry (HARDWARE / errata; cited specific AMD erratum numbers -- UNVERIFIED, possibly hallucinated) | TSC-deadline reprogrammed to a time in the PAST after long C6 residency -> self-retriggering timer-interrupt storm -> the timer ISR re-enters and overflows its stack -> #PF -> #DF -> triple fault (SOFTWARE; given with buggy + fixed C code) |
| Proposed fix | disable C6 / microcode (hardware-side) | clamp the deadline to a minimum future offset + coalesce missed ticks + IST-isolate the timer-ISR stack (kernel-side, directly testable) |

**Honest verdict: Opus alone won this round.** ~11x faster, ~9x cheaper, ~7x
leaner -- and arguably the better engineering answer. It followed correct debugging
methodology (it *explicitly* said "exhaust the software bug before blaming silicon
errata"), produced a concrete, testable kernel-side fix with actual code, and its
read of the sane-RIP condition ("the ISR isn't corrupt -- it is being RE-ENTERED
faster than it returns") is cleaner than Fusion's nested-exception speculation.
Fusion was more elaborate and cited specific errata numbers, which *look*
authoritative but are unverified and may be confabulated; its web-search appears to
have ANCHORED the panel onto a hardware/errata explanation, pulling it away from the
simpler software bug Opus found.

**Design takeaway:** this is exactly why Fusion is the APEX tier -- a last resort
after Opus + Codex have demonstrably failed -- and NOT an upgrade over Opus. A
single strong model is faster, cheaper, and (on a problem it can actually reason
about) often better. Escalate to the expensive diverse panel only when the cheaper
tiers have struck out. Elaborateness is not correctness.
