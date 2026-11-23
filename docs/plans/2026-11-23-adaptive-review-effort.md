# Adaptive Review Effort Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Astra's reasoning effort per pass comes from `.conclave/reviewers.toml`, by area tier and by the chain's last finding, and is applied by the dispatcher and recorded with its tier on every receipt.

**Architecture:** `review_pin.py` validates a new `[schedule.effort]` table and answers `--effort KIND` for a section from the reviews manifest through its shared manifest reader. `scripts/codex-dispatch.sh` asks it once the review range is settled and passes `--effort` to a vendored, patched copy of OpenAI's Codex companion, then writes `effort` and `tier` on the receipt.

**Tech Stack:** Python 3 (pytest), bash, Node 18+ (`node --test`), TOML.

**Spec:** `docs/specs/2026-11-23-adaptive-review-effort-design.md`

## Global Constraints

- Levels are the companion's own list: `none, minimal, low, medium, high, xhigh`.
- Tiers, in precedence order: `deep` = `src/kernel/`, `include/kernel/`, `src/boot/`; `risky` = `src/desktop/`, `include/`, `src/libc/`, `src/libs/`, `src/installer/`, `user/`, `src/apps/`, `Makefile`, `scripts/build.sh`; `standard` = everything else. A path matches a tier when it equals an entry or starts with an entry that ends in `/`. A section takes the highest tier any changed path matches.
- Rounds 1-3: deep `high`, risky `medium`, standard `medium`. Round 4 on: deep `medium`, risky `medium`, standard `low`. Next round after a round whose highest is P1: deep `high`, risky `medium`, standard `medium`. Every kind other than `round` and `reconciliation` (today plan, performance, completion; later any new kind such as `redesign`): `medium`, and it does not move the chain. `reconciliation` counts as a round in the chain.
- A section whose checked plan receipt is older than the table's `since` gets no pinned effort for ANY kind (the Codex config value applies). Section 70 is such a section; its passes carry no `--effort`.
- The chain position is read only through `review_pin.py`'s shared manifest reader (the one `newest_receipts` uses), never a new direct call to `unreadable_line_may_be`, so the atomic-receipts section replaces it in one place.
- Claude never types `--effort`; `codex_model_flag_block.py` is unchanged and still refuses it.
- Work happens in `../impossible-os-repair` (the run is live in the primary tree). Commit with `git -C ../impossible-os-repair commit -F <msg> -- <paths>`. Landing on `main` is done by the safe-gap monitor (Task 6).
- No em or en dashes in anything written. No AI attribution in commits.
- `todo/` files are edited through Bash in this session until Task 1's hook fix reaches the primary tree (WORKAROUND: the primary tree's copy of `todo_graph_auto_rewrite.py` rewrites primary TODO files on a worktree Edit).

## Review Focus

- A section that touches no tier path (docs only): `standard`, not a refusal. Task 3.
- An unreadable manifest line newer than the section's receipts: `--effort` refuses, and the dispatcher, AFTER the base checks pass, runs with no `--effort` and says why in the transcript. An ordinary round with an unknown base still refuses before dispatch. Tasks 3 and 4.
- An owed section (built during an outage, no plan receipt): new under the table, chain at 0. Task 3.
- A pin with no `[schedule.effort]` table: every pass runs with no `--effort`, exactly as today. Tasks 3 and 4.
- A round still running (receipt not checked): only checked receipts count. Task 3.

---

### Task 1: The TODO auto-rewrite hook repairs the edited file's own worktree (DONE)

Commits `6f79a36ff`, `94e9a551f`: root from the edited path, acting only on a tree whose git common directory is the hook's own (plan review P1). Regressions: a linked worktree is repaired there; an unrelated repository runs none of its code. `scripts/tests/test_todo_graph_auto_rewrite.py` 12/12; both new cases fail on the earlier versions.

### Task 2: Vendor the Codex companion with `--effort` on adversarial-review

**Files:**
- Create: `tools/codex-companion/` = upstream `plugins/codex/{scripts,prompts,schemas,LICENSE,NOTICE}` plus `tests/{fake-codex-fixture.mjs,helpers.mjs}` from `openai/codex-plugin-cc` at `807e03ac9d5aa23bc395fdec8c3767500a86b3cf` (v1.0.4), and `tools/codex-companion/PATCHES.md` (the patch, and the upstream PR once open).
- Modify: `tools/codex-companion/scripts/codex-companion.mjs`: `handleReviewCommand` takes `effort` as a value option only when `config.reviewName === "Adversarial Review"`, else refuses `--effort` before creating a job; normalize with `normalizeReasoningEffort`; `executeReviewRun` forwards `effort` to `runAppServerTurn` on the adversarial path.
- Modify: `src/libs/PROVENANCE.md`, `CREDITS.md` (one row each: upstream URL, commit, Apache-2.0, vendoring commit)
- Test: `tools/codex-companion/tests/effort.test.mjs`

- [ ] **Step 1: Copy** byte-exact; commit alone (`vendor: OpenAI Codex companion 1.0.4, unmodified`).
- [ ] **Step 2: Failing tests** (shape of upstream `runtime.test.mjs` line 630): `adversarial review forwards reasoning effort to turn/start` (`--effort high`, `fakeState.lastTurnStart.effort === "high"`); `adversarial review without --effort sends null`; `adversarial review rejects an unknown effort` (non-zero exit, stderr `/Unsupported reasoning effort/`); `native review refuses --effort` (`review --effort high` exits non-zero and the fake saw no `review/start`).
- [ ] **Step 3: Run** `node --test tools/codex-companion/tests/`; expected FAIL on the first and fourth.
- [ ] **Step 4: Patch** as in Files.
- [ ] **Step 5: Run**; expected 4 PASS. Read the whole vendored tree (imported-code rule) and note anything surprising in `PATCHES.md`.
- [ ] **Step 6: Commit** `vendor: the Codex companion forwards --effort on adversarial-review` with the PROVENANCE and CREDITS rows.

### Task 3: The pin's effort table and `review_pin.py --effort`

**Files:**
- Modify: `.conclave/reviewers.toml` (table below; replace the "There is no `effort` key here" comment with two lines: effort is set here by tier and finding, decided 2026-11-23, spec path)
- Modify: `scripts/review_pin.py`
- Test: `scripts/overnight/tests/test_review_pin.py`

```toml
[schedule.effort]
since = <unix seconds when the table is committed>
passes = "medium"
held_rounds = 3

[[schedule.effort.tiers]]
name = "deep"
paths = ["src/kernel/", "include/kernel/", "src/boot/"]
rounds = "high"
later = "medium"
after_p1 = "high"

[[schedule.effort.tiers]]
name = "risky"
paths = ["src/desktop/", "include/", "src/libc/", "src/libs/", "src/installer/", "user/", "src/apps/", "Makefile", "scripts/build.sh"]
rounds = "medium"
later = "medium"
after_p1 = "medium"

[[schedule.effort.tiers]]
name = "standard"
paths = []
rounds = "medium"
later = "low"
after_p1 = "medium"
```

**Interfaces:**
- Produces: `load_pin(...)["effort"]` = `None` or `{"since": int, "passes": str, "held_rounds": int, "tiers": ((name, paths, rounds, later, after_p1), ...)}` in pin order; `tier_for(pin, changed_paths) -> str | None`; `effort_for(pin, manifest, todo, section, kind, changed_paths) -> str | None`; `section_chain(manifest, todo, section, since_ts) -> (count, newest_highest)` in the shared reader; CLI `--effort KIND --todo TODO --section-n N --manifest FILE --paths-file0 FILE` prints `<level> <tier>`, or nothing (exit 0) when there is no table or the section predates `since`; exit 2 (Refused) on an unreadable newer line.

- [ ] **Step 1: Failing tests:**
  - `test_effort_table_validates`: repo pin loads, `held_rounds == 3`; an unknown level, a last tier with non-empty `paths`, an unknown key, `held_rounds = 0` each raise `rp.Refused`.
  - `test_effort_absent_is_none`: no table gives `effort is None`, `effort_for(...) is None`.
  - `test_tier_precedence`: `include/kernel/a.h` deep; `include/gfx.h` + `docs/x.md` risky; `user/x.c` risky; `Makefile` risky; `Makefile.bak` standard; `docs/x.md` and `()` standard.
  - `test_round_ladder_deep`: 0, 1, 2 checked prior rounds give `high`; 3 prior (last P2) `medium`; last P1 `high`; a P2 after it `medium`.
  - `test_round_ladder_standard`: rounds 1-3 `medium`; round 4 after a P3 `low`; after a P1 `medium`.
  - `test_reconciliation_counts_as_round`: 2 rounds + 1 reconciliation, next reconciliation is round 4.
  - `test_other_kinds_take_passes_level`: plan, performance, completion and an unknown future kind name give `medium` in every tier, and none of them moves the chain count.
  - `test_unchecked_rounds_do_not_count`.
  - `test_section_before_since_gets_nothing_for_every_kind`: plan receipt `ts < since` gives `None` for plan, round, performance, completion, reconciliation.
  - `test_owed_section_without_plan`: round 1 level.
  - `test_unreadable_newer_line_refuses`.
  - `test_cli_effort`: prints `high deep` for a fresh deep round; nothing for a pin without the table.
- [ ] **Step 2: Run** `pytest scripts/overnight/tests/test_review_pin.py -k "effort or tier" -v`; expected FAIL.
- [ ] **Step 3: Implement.** Eligibility first (table present, plan receipt absent or `ts >= since`), then kind, then chain from `section_chain`, which reads with the same newest-first loop and unreadable-line rule as `newest_receipts`. Level = `rounds` while count `< held_rounds`, else `after_p1` when the newest highest is `P1`, else `later`.
- [ ] **Step 4: Run** the file and `bash scripts/overnight/tests/run-all.sh`; expected PASS.
- [ ] **Step 5: Commit** `review: the pin sets reviewer effort by area tier and the chain's last finding`, `since` set from `date +%s` just before the commit.

### Task 4: The dispatcher applies the effort and records it

**Files:**
- Modify: `scripts/codex-dispatch.sh`: default `COMPANION` (line 128) to `<repo root>/tools/codex-companion/scripts/codex-companion.mjs`; ask `--effort` after every base and range check has passed (after the `PIN_LENSES` step at line 285 and the reconciliation range); keep the `<level> <tier>` answer in `EFFORT`/`TIER`; a non-zero answer leaves both empty and holds the reason in a variable that is written to the transcript once it opens (line 349); line 383 becomes `node "$COMPANION" adversarial-review ${EFFORT:+--effort "$EFFORT"} -- "$PROMPT"`; the receipt (line 427) gains `"opt:effort=${EFFORT:-}" "opt:tier=${TIER:-}"`; line 456 unchanged.
- Modify: `scripts/overnight/review-broker-codex-dispatch.sh:180` (same default companion)
- Test: `scripts/overnight/tests/test_review_pin.py` (its dispatcher tests drive `codex-dispatch.sh` with a fake `CODEX_COMPANION_PATH`; the fake appends its argv to a file)

- [ ] **Step 1: Failing tests:** `test_dispatch_passes_pinned_effort` (fresh round on a deep change: argv has `--effort high` before `--`, receipt `effort: high`, `tier: deep`); `test_dispatch_without_table_passes_no_effort` (no `--effort`, no `effort`/`tier` keys); `test_dispatch_effort_refusal_falls_back` (an OWED round whose recorded base is readable, plus an unreadable line newer than its receipts: the review runs, no `--effort`, transcript holds `effort unknown`); `test_dispatch_unknown_base_still_refuses` (an ordinary round with an unreadable line after its plan exits 2 before the companion runs); `test_default_companion_is_vendored` (grep of the script text for the new default).
- [ ] **Step 2: Run**; expected FAIL except the base-refusal test, which already passes (it pins existing behaviour).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the file, `bash scripts/overnight/tests/run-all.sh`, `bash scripts/lint.sh`; expected PASS.
- [ ] **Step 5: Commit** `review: the dispatcher passes the pinned effort and writes it with its tier on the receipt`.

### Task 5: Record and docs

**Files (todo/ through Bash):**
- `todo/00-infrastructure/TODO-08-automation-hardening.md`: section 70's threat-model item is replaced by an XREF line to the review-depth section's host-tooling threat-model item; its measurement item becomes `- [/]` naming its blocker ("waits for two weeks of receipts, 2026-12-07"); add `tier` to the receipt item; in section 62, the `deep_paths` item is reworded to "the deep set is every tier above `standard` in `[schedule.effort]`; no second path list" with a reciprocal XREF to section 70. Then `bash scripts/todo-graph/build-and-validate.sh --keep-cache`, `python3 scripts/todo-plan.py --sync`.
- `CLAUDE.md` Codex Invocation Policy (effort is the operator's pin, applied by the dispatcher after the base checks; Claude never passes or chooses it; the Codex config value is the fallback and covers sections started before the table), `docs/infrastructure/codex-invocation-policy.md` (same, plus the vendored companion), `.claude/skills/three-pass-review.md` (one line: effort and tier come from the pin and are on the receipt).
- The docs page covering TODO-08 (`grep -rl "covers=todo/00-infrastructure/TODO-08" docs`): bump `reviewed=`.

- [ ] **Step 1: Edit.** **Step 2: Verify** `bash scripts/lint.sh`, `python3 scripts/site/build.py --check`, `python3 scripts/todo-plan.py --check`; expected clean. **Step 3: Commit** `todo, docs: reviewer effort is the pin's, by area and finding`.

### Task 6: Review, upstream, stamp, land

Order: the plan pass is redispatched on this revised plan before Task 2.

- [ ] **Step 1: Astra review** of section 70 through `.claude/skills/three-pass-review.md`: round and performance on the candidate after Task 5, every finding fixed with a test, to a clean wave or the cap. The unmodified vendor commit is named in the prompt as imported code to read whole. Commit nothing while a pass is reading.
- [ ] **Step 2: Upstream PR** to `openai/codex-plugin-cc` from a fork, once the round is clean: the companion patch and the four tests in `tests/runtime.test.mjs`, title `adversarial-review: accept --effort like task`. Record the URL in `PATCHES.md` and section 70, tell the operator.
- [ ] **Step 3: Completion pass** on the candidate holding the PR URL; fix gaps; stamp section 70 when clean (the measurement item stays `[/]`).
- [ ] **Step 4: Land** through a safe-gap monitor: wait until the run has no unpushed commits and no push or suite in flight, rebase onto `origin/main`, push `HEAD:main` in the background, `python3 scripts/overnight/ci-wait.py .` until green, then remove the worktree.
- [ ] **Step 5: First live check:** the first receipt written after the land for a section started after `since` carries `effort` and `tier`; record it in section 70.
