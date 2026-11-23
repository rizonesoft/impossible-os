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
| `risky` | `src/desktop/`, `include/` (outside `include/kernel/`), `src/libc/`, `src/libs/`, `src/installer/`, `user/`, `src/apps/`, `Makefile`, `scripts/build.sh` | medium | medium | medium |
| `standard` | everything else (`scripts/`, `.claude/`, `todo/`, `docs/`, `gh-pages/`, `src/shell/`) | medium | low | next round medium |

- A P1 lifts only the next round. A round with no P1 returns the chain to its round-4 level.
- A `reconciliation` pass takes the same level a round would take at that point in the chain.
- My call: from round 4, a P1 lifts the next round only. Stepping down after two quiet rounds, as first sketched under C, would keep the deep tier at high through long runs of P2s, which costs more than the operator's "kernel high for the first 3 rounds" intends.
- My call: `include/` outside `include/kernel/` is `risky`, because those headers are the ABI user and kernel code share. `user/`, `src/apps/`, the top-level `Makefile` and `scripts/build.sh` are `risky` too, because the planned review-depth-per-path section already names them among the paths where a missed bug costs most (below). The control-plane scripts stay `standard`: they are where the long chains run, and their chains were long because of what the reviewer was chasing, not because effort was too low (see Related fix).

## How it is built

- **The pin owns effort.** `.conclave/reviewers.toml` gains a `[schedule.effort]` table holding the three tiers above: paths, the round 1-3 level, the round-4 level, the level after a P1, and the level for the other passes. `review_pin.py` validates it (levels from the companion's own list: `none, minimal, low, medium, high, xhigh`) and gains an `--effort` query that answers from the section's manifest history: the rounds since the section started, and the worst finding of the last round.
- **The dispatcher passes it, Claude never does.** The level is chosen only after the review range is settled (the base checks run first and still refuse an unknown base); when the level cannot be read the pass runs with no `--effort` and the transcript says why. `scripts/codex-dispatch.sh` asks `review_pin.py --effort` and hands the level to the companion. The command Claude types is unchanged, so `codex_model_flag_block.py` keeps refusing an effort flag typed by Claude. CLAUDE.md and `docs/infrastructure/codex-invocation-policy.md` change from "effort is the operator's Codex setting" to "effort is the operator's pin; the dispatcher applies it; the Codex config value is the fallback when the pin has no effort table".
- **Every receipt records its effort and tier.** Each manifest line gains `effort` and `tier`, from the same classification, so the ledger can split results by area.
- **The companion is vendored and patched.** OpenAI's Codex plugin (v1.0.4, Apache-2.0) has no `--effort` on `adversarial-review`, although its `task` command and the app-server's `turn/start` both take one. A pinned copy goes into the repo with a small patch that accepts `--effort` on `adversarial-review` only and forwards it to `runAppServerTurn`; the native `review` command refuses it, since `review/start` cannot apply it. `codex-dispatch.sh` already honours `CODEX_COMPANION_PATH`, and its default moves to the vendored copy. The import gets rows in `src/libs/PROVENANCE.md` and `CREDITS.md`, a Claude read and an Astra pass, the same as any imported code.
- **Upstream.** A pull request on OpenAI's plugin repository adds `--effort` to `adversarial-review` (operator approved). When a release carries it, the vendored copy is dropped and the default goes back to the stock plugin.
- **Sections in flight keep their rules.** A section whose plan pass predates the table runs every pass, of every kind, at the Codex config value to the end. That includes the section that builds this change, so the first live check is the next section started after it ships.

## Fit with the other planned review changes

Checked 2026-11-23 against every open review section in the automation-hardening TODO:

- [Review depth per path](../../todo/00-infrastructure/TODO-08-automation-hardening.md#62-review-depth-per-path) planned its own `deep_paths` list. The pin keeps ONE path classification: that section's deep set is every tier above `standard` in `[schedule.effort]`, and its item is reworded to say so. Its pinned threat model for host tooling is the same work as this design's Related fix, so that fix is delivered there, not here.
- [Receipts written atomically](../../todo/00-infrastructure/TODO-08-automation-hardening.md#60-review-receipts-are-written-atomically) removes the tolerant manifest reader. The chain position is read only through `review_pin.py`'s shared manifest reader, so that change replaces it in one place.
- [A repeated finding dispatches a redesign pass](../../todo/00-infrastructure/TODO-08-automation-hardening.md#63-a-repeated-finding-dispatches-a-redesign-pass) adds a pass kind. Any kind other than `round` and `reconciliation` takes the `passes` level and does not move the chain.
- [Lens yield is measured](../../todo/00-infrastructure/TODO-08-automation-hardening.md#66-lens-yield-is-measured) and [Review attempts are bound end to end](../../todo/00-infrastructure/TODO-08-automation-hardening.md#69-review-attempts-are-bound-end-to-end) read and bind the same receipts; the `effort` and `tier` fields sit beside theirs.

## Measuring it

Two weeks after the first section starts on the new schedule (review date 2026-12-07), one ledger query compares rounds per section and late-round P1 rate against the 2026-11-23 baseline above, split by tier and effort. If `standard` at low shows longer chains or P1s found only after a lift, its round-4 level goes back to medium. This is one filed item, not a dashboard.

## Related fix, delivered by review depth per path

Tooling chains run long because the reviewer chases contrived bypasses of heuristic gates: [the section-commit gate test wiring](../../todo/00-infrastructure/TODO-08-automation-hardening.md#34-section-commit-gate-test-wiring-that-kernel-sources-can-satisfy) ran 22 rounds on them. Every pass on host tooling quotes a pinned threat model. That is a prompt change, not an effort change, and the review-depth-per-path section already owns it.

## Non-goals

- No per-lens effort and no change of model: Astra stays the only reviewer.
- No change to `blocking`, `pass_cap` or the lenses.
- No effort choice by Claude at any point.
