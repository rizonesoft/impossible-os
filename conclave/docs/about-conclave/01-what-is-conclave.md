# What Conclave is

Conclave is a self-improving, cross-project **reasoning harness that behaves like its
own model**. It exposes one stable interface, `ask`, and internally runs a
model-agnostic pipeline: a swappable **panel** of cloud models whose answers are
fused by an Opus **judge**, wrapped in retrieval, memory, automatic learning, and
self-healing.

Its intelligence is the **harness** — retrieval, memory, structured process, and
learned policies — **not** a trained weight file. The core needs no base model and no
GPU. Panel and judge models are configuration (`conclave.toml`), never baked into code,
so they can be swapped without touching the engine.

## Role: the apex escalation tier

Conclave is the **apex escalation tier**. It is invoked when a primary agent (Claude)
and its reviewer (Codex) are both stuck, or for an explicit high-stakes adversarial
review (e.g. SMP, boot ABI, security). The project-side escalation ladder
(Claude → Codex → Conclave) lives in each consuming project, not in this repo;
Conclave is the last rung.

## Operating principles (invariants)

- **Off by default.** Escalation runs only when `CONCLAVE_ENABLED=1` and a key exists in
  the `secret` file. Absent either, `ask` returns a status, never an error.
- **Fail-open for the caller, fail-closed on spend.** A disabled/broke/unreachable
  Conclave returns a status (`disabled`, `no_key`, `over_budget`, `low_balance`,
  `unavailable`) so the caller is never blocked; but it refuses to spend below the
  credit floor or past the call budget.
- **Panels never web-search.** Web search is disabled by contract — panels hallucinate
  errata numbers, microcode revisions, and MSRs when allowed to browse. The judge is
  instructed to discard any such fabrications.
- **Only verified knowledge is stored.** `note` refuses unverified lessons, so the
  corpus is never polluted by unconfirmed guesses.
- **Model-agnostic.** Panel, judge, and embedders are listed in `conclave.toml`; the
  pipeline does not depend on any specific model.

## Why an ensemble + judge beats one model

Each panel model answers the same hard problem independently and in parallel. The
**discard-synthesis judge** (Opus 4.8) then reconciles them: it extracts consensus,
surfaces contradictions, keeps any single *unique correct insight* that only one model
found, names blind spots, discards fabricated facts, and commits to one root cause plus
a concrete fix. The judge is what makes the ensemble outperform any individual member.
