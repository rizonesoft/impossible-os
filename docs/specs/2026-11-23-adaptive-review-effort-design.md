# Review Effort: Tiers by Area, Adapting to Findings

**Goal:** Astra's reasoning effort is chosen per pass from the reviewer pin, deeper where code is risky and while a chain is still finding serious bugs, cheaper on quiet chains and low-risk areas, with overall spend about flat. Decided with the operator on 2026-11-23.

## Why

Today every pass runs at one effort, the `model_reasoning_effort = "medium"` in `~/.codex/config.toml`. The operator wants review to spend effort where it pays: more on kernel and boot, less on long quiet chains. The ledger (`.claude/overnight/reviews/manifest.jsonl`, 473 round passes over 44 sections since the four-pass schedule began on 2026-11-11) shaped the design:

- Late rounds still find P1s: rounds 26 and later returned a P1 in 32 of 87 passes, about the same rate as rounds 1-3 (21 of 122). A step-down by round number alone would drop depth exactly where bugs are still being found, so a P1 lifts effort back up.
- Most spend is on our own tooling: `00-infrastructure` averages 12.9 rounds per section (max 67), `08-graphics-ui` 8.4. Kernel and boot have two sections under the current schedule, so the kernel setting starts from the operator's judgment, not data.

## Approaches considered

| Approach | Trade-off | Outcome |
|---|---|---|
| A. Fixed by round number (medium for rounds 1-3, low from round 4, risky paths medium) | simplest; ignores that late rounds still find P1s | not chosen |
| B. Fixed tiers by area | puts depth where risk is; still steps down blindly | not chosen |
| C. Tiers by area, effort follows findings | cheap on quiet chains, full depth while P1s appear; a little dispatcher logic | **chosen by the operator** |

## The schedule

Every tier runs `plan`, `performance` and `completion` at **medium**, per the operator's sketch.

| Tier | Paths (a section takes the highest tier any changed file falls in) | Rounds 1-3 | Round 4 onward | After a round with a P1 |
|---|---|---|---|---|
| `deep` | `src/kernel/` (drivers and the gfx engine live here), `include/kernel/`, `src/boot/` | high | medium | next round high |
| `risky` | `src/desktop/`, `include/` (outside `include/kernel/`), `src/libc/`, `src/libs/`, `src/installer/` | medium | medium | medium |
| `standard` | everything else (`scripts/`, `.claude/`, `todo/`, `docs/`, `gh-pages/`, `user/`, `src/shell/`, `src/apps/`) | medium | low | next round medium |

- A P1 lifts only the next round. A round with no P1 returns the chain to its round-4 level.
- A `reconciliation` pass takes the same level a round would take at that point in the chain.
- My call: from round 4, a P1 lifts the next round only. Stepping down after two quiet rounds, as first sketched under C, would keep the deep tier at high through long runs of P2s, which costs more than the operator's "kernel high for the first 3 rounds" intends.
- My call: `include/` outside `include/kernel/` is `risky`, because those headers are the ABI user and kernel code share. The control-plane scripts stay `standard`: they are where the long chains run, and their chains were long because of what the reviewer was chasing, not because effort was too low (see Related fix).

## How it is built

- **The pin owns effort.** `.conclave/reviewers.toml` gains a `[schedule.effort]` table holding the three tiers above: paths, the round 1-3 level, the round-4 level, the level after a P1, and the level for the other passes. `review_pin.py` validates it (levels from the companion's own list: `none, minimal, low, medium, high, xhigh`) and gains an `--effort` query that answers from the section's manifest history: the rounds since the section started, and the worst finding of the last round.
- **The dispatcher passes it, Claude never does.** `scripts/codex-dispatch.sh` asks `review_pin.py --effort` and hands the level to the companion. The command Claude types is unchanged, so `codex_model_flag_block.py` keeps refusing an effort flag typed by Claude. CLAUDE.md and `docs/infrastructure/codex-invocation-policy.md` change from "effort is the operator's Codex setting" to "effort is the operator's pin; the dispatcher applies it; the Codex config value is the fallback when the pin has no effort table".
- **Every receipt records its effort.** Each manifest line gains an `effort` field, so the ledger can show what each level found.
- **The companion is vendored and patched.** OpenAI's Codex plugin (v1.0.4, Apache-2.0) has no `--effort` on `adversarial-review`, although its `task` command and the app-server's `turn/start` both take one. A pinned copy goes into the repo with a small patch that forwards `--effort` to `runAppServerTurn`. `codex-dispatch.sh` already honours `CODEX_COMPANION_PATH`, and its default moves to the vendored copy. The import gets rows in `src/libs/PROVENANCE.md` and `CREDITS.md`, a Claude read and an Astra pass, the same as any imported code.
- **Upstream.** A pull request on OpenAI's plugin repository adds `--effort` to `adversarial-review` (operator approved). When a release carries it, the vendored copy is dropped and the default goes back to the stock plugin.
- **Sections in flight keep their rules.** A section keeps the schedule it started under (the pin's existing rule), so a section started before this change runs at the Codex config value to the end.

## Measuring it

Two weeks after the first section starts on the new schedule (review date 2026-12-07), one ledger query compares rounds per section and late-round P1 rate against the 2026-11-23 baseline above, split by tier and effort. If `standard` at low shows longer chains or P1s found only after a lift, its round-4 level goes back to medium. This is one filed item, not a dashboard.

## Related fix, separate from effort

Tooling chains run long because the reviewer chases contrived bypasses of heuristic gates: [the section-commit gate test wiring](../../todo/00-infrastructure/TODO-08-automation-hardening.md#34-section-commit-gate-test-wiring-that-kernel-sources-can-satisfy) ran 22 rounds on them. Every pass on a heuristic gate names its threat model in the prompt. That is a prompt change, not an effort change, and ships alongside.

## Non-goals

- No per-lens effort and no change of model: Astra stays the only reviewer.
- No change to `blocking`, `pass_cap` or the lenses.
- No effort choice by Claude at any point.
