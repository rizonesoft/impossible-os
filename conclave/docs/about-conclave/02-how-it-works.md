# How Conclave works

The core is `harness.escalate()`. One run is a fixed pipeline:

**gate → retrieve (inject memory) → structured prompt → panel + judge → verify → metrics**

## 1. Gate (fail-open for caller, fail-closed on spend)

`escalate()` returns early with a status string instead of running, when:

- Conclave is disabled (`CONCLAVE_ENABLED` != `1`) → `disabled`
- no API key in `secret` → `no_key`
- the per-project call budget is spent (`max_calls`, tracked in
  `data/projects/<proj>/budget.json`) → `over_budget`
- remaining OpenRouter credit is below `min_credits` (the USD floor) → `low_balance`

## 2. Retrieve and inject memory

`memory.recall(brief, k=5)` runs the hybrid index (vector ANN + lexical full-text,
fused by Reciprocal Rank Fusion). Any returned lessons are prepended to the prompt as
`RELEVANT PRIOR KNOWLEDGE:` ahead of the problem. Retrieval is best-effort: if it
fails, escalation proceeds with the bare brief.

## 3. Structured prompt

A mode-specific system prompt is selected:

- `stuck` — "senior systems engineer cracking a hard problem the primary agent and its
  reviewer could not solve."
- `review` — "senior reviewer doing a high-stakes adversarial review on a critical path."

Both carry a hard guard: do **not** web-search, do **not** invent errata numbers,
microcode revisions, or undocumented MSRs — reason only from documented behavior and the
given context.

## 4. Panel (the teachers)

`panel.run()` calls every configured panel model in parallel
(`ThreadPoolExecutor`, `workers` threads), each with the system prompt + assembled
context. Each result captures content, cost, latency, and token counts. A model that
errors is recorded but does not fail the run; the run only fails if *no* model returns
usable content (`unavailable`).

## 5. Judge (discard-synthesis)

`judge.judge()` sends all successful panel analyses (plus any `prior` tier analyses
from Claude/Codex) to the judge model. It returns a single synthesis committing to the
most likely root cause and concrete fix. If the judge call fails, Conclave falls back to
returning the raw panel outputs (`panel_only`).

## 6. Verify and record metrics

The call budget is incremented, and per-run metrics are appended and folded into
cumulative totals: panel cost, judge cost, total cost, latencies (panel max/sum, judge,
wall), prompt/completion tokens, and per-model breakdown. The transcript is recorded for
later compilation.

## Return shape

`escalate()` returns a dict with `status` and, on success, `output` (the synthesis),
plus `panel`, `cost`, `cumulative_cost`, `tokens`, and `wall_s`. Success status is `ok`
(judged) or `panel_only` (judge unavailable, raw panel returned).
