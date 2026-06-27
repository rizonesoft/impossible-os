# .fusion -- OpenRouter Fusion escalation (apex tier)

The last rung of the Claude -> Codex -> Fusion escalation ladder, implemented as a
**DIY ensemble** -- each panel model called SOLO + in parallel, NO web-search, with an
Opus 4.8 discard-judge that credits unique-correct insights and throws out fabricated
errata/microcode/MSRs. Current panel: GLM 5.2 + DeepSeek V4 Pro + Kimi K2.7-code +
Grok-build + (probationary) MiniMax-M3 + Qwen3-Max. This is the config that beat lone
Opus -- see the 4-round investigation + "Production configuration" below. The main
Claude thread is the real review layer -- it reads + validates Fusion's output before
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
- `ladder.py` -- async job queue (`dispatch`/`poll`/`list`/`outcome`/`stats`).
- `config.toml` -- panel / judge / caps.
- `secret` -- your OpenRouter key (GITIGNORED, never commit).
- `dataset.jsonl` -- collected escalation in/out + outcome for evals/distillation
  (GITIGNORED, local only).
- `metrics.jsonl` -- one rich line PER RUN (GITIGNORED). Performance, cost, tokens,
  and an intelligence signal -- lean, no brief/output body. Per line:
  `latency_s {panel_max, panel_sum, judge, wall}`, `cost_usd {panel, judge,
  run_total, cumulative_total}`, `tokens {prompt, completion, total}`,
  `intelligence {panel_ok, panel_total, judged, judge_chars, outcome}`, plus
  `per_model` (cost/sec/chars/tokens each). `intelligence.outcome` starts
  `unknown` and is back-filled to `resolved`/`unresolved` when the validating
  main thread runs `ladder.py outcome <id> <verdict>` -- that verdict is the
  ground-truth quality measure (the rest is measured at run time).
- `totals.json` -- cumulative ledger (GITIGNORED): `runs`, `total_cost`
  (= `panel_cost` + `judge_cost`), `total_tokens`, `resolved`/`unresolved`,
  and `by_mode`. `python3 ladder.py stats` prints it as a one-liner. This is
  the running answer to "what has Fusion cost me in total, and how often did
  it actually resolve the problem."

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

## Round 2: frontier panel + strong judge -- the judge is what matters

Re-ran the identical heisenbug with a FRONTIER panel (Grok-build, Opus 4.8,
GPT-5.5, Nemotron-Ultra-550B, Qwen3-Max) and an **Opus 4.8 judge**. All five panel
models responded (the free Nemotron included).

| | cheap panel + GLM judge | frontier panel + Opus judge | Opus 4.8 alone |
| --- | --- | --- | --- |
| Time | 16 min | 12 min | **1.4 min** |
| Cost | $1.16 | $1.07 | **$0.13** |
| Committed root cause | HARDWARE errata (unverified) | **SOFTWARE: LAPIC/TSC-deadline timer not disarmed before C6 entry** (with fix code) | **SOFTWARE: TSC-deadline-in-past -> re-entrant timer storm -> stack overflow** (with fix code) |

**The decisive finding: the JUDGE dominated the outcome, not the panel.** In BOTH
Fusion runs the panel contained *both* hypotheses -- the software timer bug AND the
hardware-errata speculation (one frontier member committed to "erratum #1485,"
exactly like the cheap panel). The weak GLM judge anchored on the impressive-looking
errata (the worse answer); the strong Opus judge correctly down-weighted it and
committed to the actionable software fix -- the SAME root cause lone Opus found.

**But that is the trap:** the best judge (Opus) IS the baseline. Frontier-Fusion
with an Opus judge merely RE-DERIVED Opus-alone's answer -- at ~8x the cost and ~9x
the time. The panel's diversity did not beat a single strong model; it was overridden
by the judge's quality, and the strongest judge is the thing you already have.

**Reinforced conclusion:** judge strength > panel strength; and the strongest judge
is the baseline. Fusion is hard to justify over lone Opus on problems Opus can reason
about. It earns its place ONLY as the apex last-resort tier (after Opus + Codex have
demonstrably failed), where the bet is that the panel surfaces a hypothesis the
single model genuinely lacked -- and even then it needs a strong judge to pick it
out. Spending 8-9x to re-derive what Opus already says is not a win.

## Round 3: DIY ensemble, no web-search -- the first config that BEAT Opus

Ran each of 10 models SOLO and in parallel (no web-search; reasoning from
knowledge), then an Opus 4.8 judge synthesized over all of them, with a judge prompt
that EXPLICITLY asked it to flag unique-correct insights and discard hallucinations.
9/10 produced (Xiaomi MiMo returned null content). ~7 min wall-clock, **$1.08** total.

| model | cost | sec | chars |
| --- | --- | --- | --- |
| openai/gpt-5.5 | $0.355 | 297 | 25053 |
| google/gemini-3.5-flash | $0.180 | 121 | 13506 |
| moonshotai/kimi-k2.7-code | $0.079 | 354 | 13790 |
| x-ai/grok-build-0.1 | $0.031 | 147 | 9092 |
| minimax/minimax-m3 | $0.019 | 194 | 17320 |
| qwen/qwen3-max | $0.009 | 44 | 8762 |
| deepseek/deepseek-v4-flash | $0.0005 | 98 | 9974 |
| nvidia/nemotron-3-ultra-550b (free) | $0 | 137 | 14101 |
| openrouter/owl-alpha (free) | $0 | 390 | 44224 |
| xiaomi/mimo-v2.5 | ERR (null content) | - | - |
| **Opus judge** | **$0.402** | 39 | 6260 |

**This time the ensemble genuinely beat lone Opus.** The Opus judge:

1. **Caught the hallucinations and discarded them** -- Nemotron's fabricated
   TSC-drift math ("physically wrong: Zen 4 has invariant TSC; siblings share the
   same core TSC"), Owl's confabulated microcode version `0x0A704104` and named
   errata, and noted "several analysts invented errata numbers ... the dominant
   failure mode."
2. **Preserved THREE unique-correct insights lone Opus did NOT produce:**
   - **Gemini:** `MWAIT` executed with `IF=1` -> the wake interrupt vectors straight
     into the ISR, *bypassing the post-MWAIT fixup* (state revalidation) -- a real,
     often-overlooked from-scratch-kernel mechanism.
   - **Kimi:** the *same* stale GS_BASE/CR8/APIC state that breaks the ISR also
     breaks `#DF` delivery -- which is *why* you get a triple fault, not a clean
     panic (explains the valid-RIP/sane-state clue best).
   - **GPT-5.5:** the decisive A/B test -- enable C6 but route deep-idle wake through
     HPET/PMTMR broadcast with the LAPIC timer masked; 24h survival confirms the bug
     class without disabling SMT or swapping hardware.
   - Plus the blind spot nobody else raised: a from-scratch kernel almost certainly
     lacks ACPI `_CST`/`_CSD` coordination, making deep C-states unsafe by construction.
3. **Synthesized a more complete, better-grounded root cause than Opus-alone** --
   fusing GPT-5.5's framing + Gemini's IF=1 insight + Kimi's poisoned-`#DF` mechanism,
   with NO invented errata.

**Why this config won where the plugin runs lost:** (a) **no web-search** -- the
plugin's auto-search anchored earlier panels on confabulated errata; reasoning from
knowledge avoided it; (b) a **strong judge explicitly tasked** to surface
unique-correct insights and discard hallucinations, not the plugin's opaque judge;
(c) a genuinely diverse strong panel. **But the value concentrated in the strong
models** (GPT-5.5, Gemini, Kimi) -- the cheap/exotic members (Nemotron, owl,
deepseek-flash, minimax) mostly produced the hallucinations the judge discarded.

## Updated design conclusion

Build Fusion as a **DIY ensemble, NOT the `openrouter/fusion` plugin:**
1. Call each panel model SOLO (per-model responses + costs = the metrics; no plugin opacity).
2. **No web-search** (it anchors models onto hallucinated errata/microcode/MSRs).
3. A small panel of **3-4 genuinely strong, diverse** models (GPT-5.5 + Gemini + Kimi
   were the value; a big bag of cheap models just adds noise).
4. A **strong judge** (Opus) with an explicit prompt to extract unique-correct
   insights and DISCARD hallucinations.
5. The main Opus thread remains the final review layer.

Done this way the ensemble surfaced complementary correct mechanisms a single Opus
missed, at ~$1 / ~7 min -- a real (if narrow) win. The earlier "just re-derives
Opus" result was an artifact of web-search anchoring + an opaque judge, not a
property of ensembles per se.

## Round 4: lean 5-model panel + discard-judge -- the best result, and the cheapest

Panel = GPT-5.5 + Grok-build + Gemini-3.5-flash + DeepSeek-V4-Pro + GLM 5.2 (the
noisy exotics from Round 3 dropped), judge = Opus 4.8 with an explicit "discard
fabricated errata/microcode/MSRs" instruction. All 5 produced. ~6 min wall-clock,
**$0.92** total (cheaper than every prior run).

This produced the strongest synthesis of the whole investigation. The Opus judge:

- Built a **root-cause family table** (A: stale per-CPU data after C6 / B: TLB-shootdown
  race / C: MMIO cacheability / D: microarch-electrical) and reasoned *between* them.
- Used the **"sane CR2" clue to DEMOTE the MMIO family** ("an MMIO fault would show
  CR2 ~0xFEE00000, not a sane kernel address") and promote the stale-data family --
  a genuinely sharp deduction.
- **Surgically discarded fabrications while keeping real facts**: it threw out
  Gemini's invented microcode gate, its magic `80us->150us` latency numbers, and its
  "SMU SRAM TSS state-restore race" / VRM-droop narrative as confabulated -- while
  explicitly *keeping* the real ones (`IA32_TSC_DEADLINE=0x6E0`, ARAT
  `CPUID.06H:EAX[2]`). That precision is exactly the point of a strong discard-judge.
- **Credited each model's unique correct insight**: GLM's bare-`lfence` discriminator
  test (does a zero-I/O barrier mask it? -> proves memory-ordering), GPT-5.5's x2APIC
  switch (excises the whole MMIO family in one config change), DeepSeek's SMT-sibling
  TLB-shootdown IPI-mask bug, Grok's TSC-deadline-clamp-on-C-state-exit.
- Committed to **Family A** (missing memory-ordering/context-re-establishment barrier
  on the Zen 4 C6 exit path -> timer ISR runs against a stale per-CPU view -> #PF ->
  #DF -> triple fault), with a **prioritized fix**: Step 0 survive-the-fault (IST
  stacks + survives-reset crash record), Step 1 the one-line `lfence` discriminator,
  Step 2 the real barrier + ARAT/broadcast + deadline clamp, Step 3 TLB-IPI-mask fix,
  Step 4 x2APIC + UC-mapping audit.

**This beat lone Opus, the 10-model panel, AND every Fusion-plugin run -- at the
lowest cost.** Gemini was the weak link (its specifics were fabricated and
discarded); the value came from GLM/GPT-5.5/DeepSeek/Grok. A tight panel of strong
diverse models gives the judge less noise to filter and a higher signal to fuse.

## Production configuration (what the ladder should actually use)

- **DIY ensemble, not the `openrouter/fusion` plugin.** Solo parallel calls -> per-model
  metrics, no opacity, no auto web-search.
- **No web-search** (it anchors models onto confabulated errata).
- **A lean panel of strong, diverse models that are NOT already in the ladder.**
  Since Opus (Tier 0) and Codex/GPT-5.5 (Tier 1) have already run by the time Fusion
  fires, they are NOT in the panel; instead their already-produced analyses are **fed
  into the judge** via `escalate(prior=[...])` -- the ladder's outputs flow forward
  into the apex (GPT-5.5's value, e.g. the x2APIC idea, arrives via Codex's Tier-1
  output). Shipped panel: **GLM 5.2 + DeepSeek V4 Pro + Kimi K2.7-code + Grok-build**
  (the credited contributors), plus **MiniMax-M3 + Qwen3-Max on probation** for
  unseen-problem diversity. Probationary members are kept/pruned by the per-model
  dataset metrics (credited-insight rate) over many real escalations -- not by any
  single test.
- **Strong judge (Opus)** with an explicit prompt to extract unique-correct insights
  and DISCARD fabricated errata/microcode/MSRs.
- **The main Opus thread is the final review layer** (validates before applying).
- Cost ~$0.6-0.9, ~6 min, async-dispatched. Beats lone Opus on genuinely hard
  problems; reached only as the apex tier after Opus + Codex have failed.
