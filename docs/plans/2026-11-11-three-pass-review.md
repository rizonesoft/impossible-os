# Three-Pass Review Across Three Repositories

**Goal:** Impossible OS, RasterFold and the Conclave SDK read one reviewer pin format through one loader, with the five improvements the operator approved on 2026-11-11. Design for this repository: [docs/specs/2026-11-11-three-pass-review-design.md](../specs/2026-11-11-three-pass-review-design.md).

## What was found

- **The SDK kit is behind RasterFold.** `D:\Conclave-SDK\kits\reviewer\templates\scripts\review-dispatch.py` is 859 lines with the older rotation schedule (three rounds at falling effort, then one lens per round). RasterFold's copy is 1,176 lines and added the three-pass policy, reconciliation and the pass cap after it took the kit. The two differ in 579 lines. The SDK has no three-pass policy at all.
- **So "share one loader" starts by moving three-pass upstream.** RasterFold's dispatcher is the newer source; the SDK kit receives it, then both projects take the kit's copy.
- **Impossible OS cannot take the SDK dispatcher whole.** It dispatches through the Codex companion and its hooks forbid model and effort flags. What it can share is the pin loader: the file that validates the pin and answers which lenses, caps and blocking levels apply.

## The five improvements

| # | Improvement | Impossible OS | SDK kit | RasterFold |
|---|---|---|---|---|
| 1 | Each lens has one owner; completion owns `plan-delivery` and `record` | done in the pin and loader | to do | to do |
| 2 | One machine-checked verdict line per lens | loader done; recorder wiring to do | to do | to do |
| 3 | Lenses added by changed path (`[[schedule.conditional]]`) | done in the pin and loader | to do | to do, replaces `--surface` |
| 4 | `blocking` severity levels in the pin | done in the pin and loader | to do | to do |
| 5 | `pass_cap`, optional `effort`, and one shared loader | loader written here first | receives the loader | takes the kit copy |

## Shared loader

`scripts/review_pin.py` is the shared file. It is standard-library Python, has no project paths in its validation, and is copied into each repository, never linked: no hardlink, no symlink, no submodule. A project records the SDK commit it copied from and owns its copy from then on, as the SDK's own rules require.

Project rules stay outside the shared file. Impossible OS adds three in a local layer: one writer and one route, no `effort` key, and the pinned model must match `~/.codex/config.toml`.

Before it goes to the SDK the loader gains what the other two repositories need and Impossible OS does not: several routes, the `agy` and `claude` reviewer CLIs, the rule that a reviewer is never of the writer's model family, an optional `effort` per reviewer and per pass, and reading the older rotation schedule so sections already under review finish on the pin they started with.

## Order

1. **Impossible OS:** land the mandate fixes and the commit gate fix, then wire the pin into the recorder, dispatcher, commit gate and round guard, replay the combined round on three shipped sections, review, push. Sections under review finish on the old rules.
2. **Conclave SDK:** through its own `todo/` lifecycle. Port three-pass and reconciliation from RasterFold into the kit dispatcher, move pin handling into `review_pin.py`, add improvements 1 to 5, update the template pin, the kit README, the todo kit's review skills and stamp rules, and the `--check-pin --json` contract the VS Code extension reads. Reinstall the SDK's own copy of the kits.
3. **RasterFold:** through its own section lifecycle. Take the kit's dispatcher and loader as copies, keep its local routes (`codex` to `carbon`), move `REVIEW_PASS_CAP` to `pass_cap = 33` in the pin, and apply improvements 1 to 4.
4. **Impossible OS:** replace its loader with the SDK's released copy and record the SDK commit.

Steps 2 and 3 are the operator's, run in sessions started in those repositories (operator decision 2026-11-11): each loads its own skills and commit hooks. This repository's sessions change neither. Until step 2 releases a loader, Impossible OS keeps its own.

## Limits

- Each repository's change is reviewed by GPT-6 Astra under that repository's rules, so this is three review cycles, not one.
- Until step 4, Impossible OS runs the first version of the loader and the other two run their current dispatchers. The pin format is the same from step 1; only the code reading it differs.
