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
- Eligibility is a marker, not a clock: the new dispatcher writes `"schedule": "effort"` on every plan receipt while the pin has an effort table. A section is on the effort schedule only when its EARLIEST checked plan receipt carries the marker; a section with a plan receipt and no marker is grandfathered for ANY kind (the Codex config value applies), and a plan retry never changes that. A section with no plan receipt (owed) is decided by its late plan pass, which the new dispatcher marks. Section 70 is grandfathered; its passes carry no `--effort`.
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

Commits `6f79a36ff`, `94e9a551f`, `dc18dd760`: root from the edited path, acting only on a registered worktree of the repository the hook is installed in (plan reviews 1 and 2, P1). Regressions: a linked worktree is repaired there; an unrelated repository and an unregistered tree pointing at this repository's metadata run none of their code. `scripts/tests/test_todo_graph_auto_rewrite.py` 12/12; each new case fails on the version before it.

### Task 2: Vendor the Codex companion with `--effort` on adversarial-review

**Files:**
- Create: `tools/vendor/codex-companion/` = upstream `plugins/codex/{.claude-plugin,scripts,prompts,schemas,LICENSE,NOTICE}` (`scripts/lib/app-server.mjs` reads `.claude-plugin/plugin.json` at load) plus `tests/{fake-codex-fixture.mjs,helpers.mjs}` from `openai/codex-plugin-cc` at `807e03ac9d5aa23bc395fdec8c3767500a86b3cf` (v1.0.4), and `tools/vendor/codex-companion/PATCHES.md` (the patch, and the upstream PR once open).
- Modify: `tools/vendor/codex-companion/scripts/codex-companion.mjs`: `handleReviewCommand` takes `effort` as a value option only when `config.reviewName === "Adversarial Review"`, else refuses `--effort` before creating a job; normalize with `normalizeReasoningEffort`; `executeReviewRun` forwards `effort` to `runAppServerTurn` on the adversarial path.
- Modify: `src/libs/PROVENANCE.md`, `CREDITS.md` (one row each: upstream URL, commit, Apache-2.0, vendoring commit)
- Test: `tools/vendor/codex-companion/tests/effort.test.mjs`

- [ ] **Step 1: Copy** byte-exact; run `node scripts/codex-companion.mjs --help` from the copy to prove it starts; commit alone (`vendor: OpenAI Codex companion 1.0.4, unmodified`).
- [ ] **Step 2: Failing tests** (shape of upstream `runtime.test.mjs` line 630): `adversarial review forwards reasoning effort to turn/start` (`--effort high`, `fakeState.lastTurnStart.effort === "high"`); `adversarial review without --effort sends null`; `adversarial review rejects an unknown effort` (non-zero exit, stderr `/Unsupported reasoning effort/`); `native review refuses --effort` (`review --effort high` exits non-zero and the fake saw no `review/start`).
- [ ] **Step 3: Run** `node --test tools/vendor/codex-companion/tests/`; expected FAIL on the first and fourth.
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
- Produces: `load_pin(...)["effort"]` = `None` or `{"passes": str, "held_rounds": int, "tiers": ((name, paths, rounds, later, after_p1), ...)}` in pin order; `tier_for(pin, changed_paths) -> str | None`; `effort_for(pin, manifest, todo, section, kind, changed_paths) -> str | None`; `section_chain(manifest, todo, section) -> (on_schedule: bool | None, count, newest_highest)` (`None` when the section has no checked plan receipt) in the shared reader; CLI `--effort KIND --todo TODO --section-n N --manifest FILE --paths-file0 FILE` prints `<level> <tier>`, or nothing (exit 0) when there is no table or the section is grandfathered; exit 2 (Refused) on an unreadable newer line.

- [ ] **Step 1: Failing tests:**
  - `test_effort_table_validates`: repo pin loads, `held_rounds == 3`; an unknown level, a last tier with non-empty `paths`, an unknown key, `held_rounds = 0` each raise `rp.Refused`.
  - `test_effort_absent_is_none`: no table gives `effort is None`, `effort_for(...) is None`.
  - `test_tier_precedence`: `include/kernel/a.h` deep; `include/gfx.h` + `docs/x.md` risky; `user/x.c` risky; `Makefile` risky; `Makefile.bak` standard; `docs/x.md` and `()` standard.
  - `test_round_ladder_deep`: 0, 1, 2 checked prior rounds give `high`; 3 prior (last P2) `medium`; last P1 `high`; a P2 after it `medium`.
  - `test_round_ladder_standard`: rounds 1-3 `medium`; round 4 after a P3 `low`; after a P1 `medium`.
  - `test_reconciliation_counts_as_round`: 2 rounds + 1 reconciliation, next reconciliation is round 4.
  - `test_other_kinds_take_passes_level`: plan, performance, completion and an unknown future kind name give `medium` in every tier, and none of them moves the chain count.
  - `test_unchecked_rounds_do_not_count`.
  - `test_grandfathered_section_gets_nothing_for_every_kind`: an earliest plan receipt without the marker gives `None` for plan, round, performance, completion, reconciliation, even when a newer plan retry carries the marker.
  - `test_marked_section_is_on_schedule`: an earliest plan receipt with the marker gives the ladder.
  - `test_owed_section_without_plan`: round 1 level.
  - `test_unreadable_newer_line_refuses`.
  - `test_cli_effort`: prints `high deep` for a fresh deep round; nothing for a pin without the table.
- [ ] **Step 2: Run** `pytest scripts/overnight/tests/test_review_pin.py -k "effort or tier" -v`; expected FAIL.
- [ ] **Step 3: Implement.** Eligibility first (table present; earliest checked plan receipt marked, or none on record), then kind, then chain from `section_chain`, which reads with the same newest-first loop and unreadable-line rule as `newest_receipts`. Level = `rounds` while count `< held_rounds`, else `after_p1` when the newest highest is `P1`, else `later`.
- [ ] **Step 4: Run** the file and `bash scripts/overnight/tests/run-all.sh`; expected PASS.
- [ ] **Step 5: Commit** `review: the pin sets reviewer effort by area tier and the chain's last finding`.

### Task 4: The dispatcher applies the effort and records it

**Files:**
- Modify: `scripts/codex-dispatch.sh`: default `COMPANION` (line 128) to `<repo root>/tools/vendor/codex-companion/scripts/codex-companion.mjs`; ask `--effort` after every base and range check has passed (after the `PIN_LENSES` step at line 285 and the reconciliation range); keep the `<level> <tier>` answer in `EFFORT`/`TIER`; a non-zero answer leaves both empty and holds the reason in a variable that is written to the transcript once it opens (line 349); line 383 becomes `node "$COMPANION" adversarial-review ${EFFORT:+--effort "$EFFORT"} -- "$PROMPT"`; the receipt (line 427) gains `"opt:effort=${EFFORT:-}" "opt:tier=${TIER:-}"`, and a plan receipt gains `"opt:schedule=effort"` whenever the pin has an effort table (`review_pin.py --has-effort` exits 0); line 456 unchanged.
- Modify: `scripts/overnight/review-broker-codex-dispatch.sh:180` (same default companion)
- Test: `scripts/overnight/tests/test_review_pin.py` (its dispatcher tests drive `codex-dispatch.sh` with a fake `CODEX_COMPANION_PATH`; the fake appends its argv to a file)

- [ ] **Step 1: Failing tests:** `test_dispatch_passes_pinned_effort` (fresh round on a deep change: argv has `--effort high` before `--`, receipt `effort: high`, `tier: deep`); `test_dispatch_without_table_passes_no_effort` (no `--effort`, no `effort`/`tier` keys); `test_dispatch_effort_refusal_falls_back` (an OWED round whose recorded base is readable, plus an unreadable line newer than its receipts: the review runs, no `--effort`, transcript holds `effort unknown`); `test_dispatch_unknown_base_still_refuses` (an ordinary round with an unreadable line after its plan exits 2 before the companion runs); `test_default_companion_is_vendored` (grep of the script text for the new default); `test_plan_receipt_marks_the_schedule` (a plan pass with the table writes `schedule: effort`, without it writes no `schedule` key); `test_section_planned_before_landing_is_grandfathered` (a plan receipt without the marker, then a round under the new dispatcher, runs with no `--effort`).
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

Order: two plan passes ran (findings folded in, see the ledger). Every pass below carries `REVIEW_BASE=4a3755ff6`, the commit before section 70's code, so its range covers the hook commits; check each receipt's `base`.

- [ ] **Step 1: Upstream PR** to `openai/codex-plugin-cc` from a fork, right after Task 2: the companion patch and the four tests in `tests/runtime.test.mjs`, title `adversarial-review: accept --effort like task`. Record the URL in `PATCHES.md` and section 70 before the review wave, and tell the operator. A review change to the patch is pushed to the PR branch.
- [ ] **Step 2: Astra review** of section 70 through `.claude/skills/three-pass-review.md`: round and performance on the candidate after Task 5, every finding fixed with a test, to a clean wave or the cap. The unmodified vendor commit is named in the prompt as imported code to read whole. Commit nothing while a pass is reading.
- [ ] **Step 3: Completion pass** on the same candidate; a repair is followed by the reconciliation wave on the repaired candidate; stamp section 70 when clean (the measurement item stays `[/]`).
- [ ] **Step 4: Land** through a safe-gap monitor: wait until the run has no unpushed commits and no push or suite in flight, rebase onto `origin/main`, push `HEAD:main` in the background, `python3 scripts/overnight/ci-wait.py .` until green, then remove the worktree.
- [ ] **Step 5: First live check:** the first receipt written after the land for a section whose earliest plan receipt is marked carries `effort` and `tier`; record it in section 70.
