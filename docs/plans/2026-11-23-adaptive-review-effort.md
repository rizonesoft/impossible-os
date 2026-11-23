# Adaptive Review Effort Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Astra's reasoning effort per pass comes from `.conclave/reviewers.toml`, by area tier and by the chain's last finding, and is applied by the dispatcher and recorded on every receipt.

**Architecture:** `review_pin.py` validates a new `[schedule.effort]` table and answers `--effort KIND` for a section from the reviews manifest. `scripts/codex-dispatch.sh` asks it and passes `--effort` to a vendored, patched copy of OpenAI's Codex companion, then writes `effort` on the receipt. Docs, the skill and the TODO record move with it.

**Tech Stack:** Python 3 (pytest), bash, Node 18+ (`node --test`), TOML.

**Spec:** `docs/specs/2026-11-23-adaptive-review-effort-design.md`

## Global Constraints

- Levels are the companion's own list: `none, minimal, low, medium, high, xhigh`.
- Tiers, in precedence order: `deep` = `src/kernel/`, `include/kernel/`, `src/boot/`; `risky` = `src/desktop/`, `include/`, `src/libc/`, `src/libs/`, `src/installer/`; `standard` = everything else. A section takes the highest tier any changed path matches (`include/kernel/x.h` is `deep`).
- Rounds 1-3: deep `high`, risky `medium`, standard `medium`. Round 4 on: deep `medium`, risky `medium`, standard `low`. Next round after a round whose highest is P1: deep `high`, risky `medium`, standard `medium`. `plan`, `performance`, `completion`: `medium` in every tier. `reconciliation` counts as a round in the chain.
- A section whose plan receipt is older than the table's `since` gets no pinned effort (the Codex config value applies).
- Claude never types `--effort`; `codex_model_flag_block.py` is unchanged and still refuses it.
- Work happens in `../impossible-os-repair` (the run is live in the primary tree). Commit with `git -C ../impossible-os-repair commit -F <msg> -- <paths>`; push only when the primary tree's `git rev-list --count origin/main..main` is 0, backgrounded, then `python3 scripts/overnight/ci-wait.py .`.
- No em or en dashes in anything written. No AI attribution in commits.
- `todo/` files are edited through Bash in this session until Task 1's hook fix reaches the primary tree (WORKAROUND: the primary tree's copy of `todo_graph_auto_rewrite.py` rewrites primary TODO files on a worktree Edit).

## Review Focus

- A section that touches no tier path at all (a docs-only section): it must get `standard`, not a refusal. Test in Task 3.
- A manifest with an unreadable line newer than the section's receipts: `--effort` must refuse (unknown chain position), and the dispatcher must then fall back to no `--effort` with a logged line, not stop the review. Test in Tasks 3 and 4.
- An owed section (built during an outage, no plan receipt): it is new under the table, round count starts at 0. Test in Task 3.
- A pin with no `[schedule.effort]` table (an older pin, or the operator removes it): every pass runs with no `--effort`, exactly as today. Test in Tasks 3 and 4.
- A round still running (receipt not checked) when the next one is asked for: only checked receipts count toward the chain. Test in Task 3.

---

### Task 1: The TODO auto-rewrite hook works on the edited file's own tree

**Files:**
- Modify: `.claude/hooks/todo_graph_auto_rewrite.py:45-49`
- Test: `scripts/tests/test_todo_graph_auto_rewrite.py` (create; or the hook's existing test file if `grep -rl todo_graph_auto_rewrite scripts/*/tests scripts/tests` finds one, then add to it)

- [ ] **Step 1: Failing test** `test_root_comes_from_the_edited_file`: make two git repos in `tmp_path` (A holding a stub `scripts/todo-graph/validate.py` that writes a marker file, B the same), run the hook with cwd=A and a payload whose `file_path` is `B/todo/x.md`; assert the marker appears in B and not in A.
- [ ] **Step 2: Run** `pytest <test> -v`; expected FAIL (marker in A).
- [ ] **Step 3: Fix** take the root from `git -C <dirname of the edited path> rev-parse --show-toplevel`.
- [ ] **Step 4: Run**; expected PASS. Then `bash scripts/audit-hooks.sh`; expected clean.
- [ ] **Step 5: Commit** `hooks: the TODO auto-rewrite hook repairs the tree that holds the edited file`. Update the 2026-11-23 bullet in memory `harness-hooks-act-on-primary-tree.md` once the run has pulled it.

### Task 2: Vendor the Codex companion with `--effort` on adversarial-review

**Files:**
- Create: `tools/codex-companion/` = upstream `plugins/codex/{scripts,prompts,schemas,LICENSE,NOTICE}` plus `tests/{fake-codex-fixture.mjs,helpers.mjs}` from `openai/codex-plugin-cc` at `807e03ac9d5aa23bc395fdec8c3767500a86b3cf` (v1.0.4), and `tools/codex-companion/PATCHES.md` (one paragraph naming the patch and the upstream PR).
- Modify: `tools/codex-companion/scripts/codex-companion.mjs` (`handleReviewCommand`: add `"effort"` to `valueOptions`, normalize with `normalizeReasoningEffort`, pass `effort` into `executeReviewRun`; `executeReviewRun`: forward `effort` to `runAppServerTurn` on the adversarial path only, since `review/start` takes none)
- Modify: `src/libs/PROVENANCE.md`, `CREDITS.md` (one row each: upstream URL, commit, Apache-2.0, vendoring commit)
- Test: `tools/codex-companion/tests/effort.test.mjs`

- [ ] **Step 1: Copy** the upstream files byte-exact; commit them alone first (`vendor: OpenAI Codex companion 1.0.4, unmodified`) so the patch is its own diff.
- [ ] **Step 2: Failing test** in upstream's style (copy the shape of upstream `runtime.test.mjs` "task forwards model selection and reasoning effort", line 630): `adversarial review forwards reasoning effort to turn/start` runs `adversarial-review --effort high <focus>` against the fake and asserts `fakeState.lastTurnStart.effort === "high"`; `adversarial review without --effort sends null` asserts `null`; `adversarial review rejects an unknown effort` asserts a non-zero exit and stderr matching `/Unsupported reasoning effort/`.
- [ ] **Step 3: Run** `node --test tools/codex-companion/tests/`; expected the first test FAILS.
- [ ] **Step 4: Patch** as in Files.
- [ ] **Step 5: Run**; expected 3 PASS. Read the whole vendored tree once (imported-code rule) and note anything surprising in `PATCHES.md`.
- [ ] **Step 6: Commit** `vendor: the Codex companion forwards --effort on adversarial-review` with the PROVENANCE and CREDITS rows.

### Task 3: The pin's effort table and `review_pin.py --effort`

**Files:**
- Modify: `.conclave/reviewers.toml` (add the table below; replace the header comment's "There is no `effort` key here" paragraph with two lines saying effort is set here, by tier and finding, decided 2026-11-23, spec path)
- Modify: `scripts/review_pin.py` (`load_pin` validation, `effort_for`, CLI)
- Test: `scripts/overnight/tests/test_review_pin.py`

Pin table (exact):

```toml
[schedule.effort]
since = <unix seconds of the commit that adds this table>
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
paths = ["src/desktop/", "include/", "src/libc/", "src/libs/", "src/installer/"]
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
- Produces: `load_pin(...)["effort"]` = `None` or `{"since": int, "passes": str, "held_rounds": int, "tiers": ((name, paths_tuple, rounds, later, after_p1), ...)}` in pin order; `effort_for(pin, manifest, todo, section, kind, changed_paths) -> str | None`; CLI `--effort KIND --todo TODO --section-n N --manifest FILE --paths-file0 FILE` prints the level, or prints nothing and exits 0 when the pin has no table or the section predates `since`; exits 2 (Refused) on an unreadable newer manifest line.

- [ ] **Step 1: Failing tests** (each builds a manifest in `tmp_path` with the existing helpers in this file):
  - `test_effort_table_validates`: the repo pin loads with `effort["held_rounds"] == 3`; a level outside the list, a missing `standard` (last tier must have `paths = []`), an unknown key, and `held_rounds = 0` each raise `rp.Refused`.
  - `test_effort_absent_is_none`: a pin without the table gives `effort is None` and `effort_for(...) is None`.
  - `test_tier_precedence`: paths `["include/kernel/a.h"]` gives deep; `["include/gfx.h", "docs/x.md"]` gives risky; `["docs/x.md"]` and `()` give standard.
  - `test_round_ladder_deep`: with 0, 1, 2 checked prior rounds the round is `high`; with 3 prior rounds (last P2) `medium`; last P1 gives `high`; then a P2 round after it gives `medium`.
  - `test_round_ladder_standard`: rounds 1-3 `medium`, round 4 with last P3 `low`, after a P1 `medium`.
  - `test_reconciliation_counts_as_round`: 2 rounds + 1 reconciliation checked, so the next reconciliation is round 4: `medium` deep, `low` standard.
  - `test_other_passes_medium`: plan, performance and completion give `medium` in all three tiers, even after a P1.
  - `test_unchecked_rounds_do_not_count`: 3 rounds of which the newest has `pass_checked: false` gives round 3: `high` deep.
  - `test_section_before_since`: a plan receipt with `ts < since` gives `None`.
  - `test_owed_section_without_plan`: no plan receipt, no rounds: round 1 level.
  - `test_unreadable_newer_line_refuses`: a garbage line after the section's receipts that `unreadable_line_may_be` accepts raises `rp.Refused`.
  - `test_cli_effort`: the CLI prints `high` for a fresh deep round and prints nothing (exit 0) for a pin without the table.
- [ ] **Step 2: Run** `pytest scripts/overnight/tests/test_review_pin.py -k effort -v`; expected FAIL.
- [ ] **Step 3: Implement.** In `load_pin`, `schedule.pop("effort", None)` before the key check, validate as the tests demand, add `"effort"` to the returned dict. `effort_for`: return `pin["effort"]["passes"]` for plan, performance and completion; for round and reconciliation, find the section's plan receipt through `newest_receipts(..., ("plan",), checked=True)` (none counts as new), return `None` if its `ts < since`, then walk `_newest_first(manifest)` counting this section's checked `round` and `reconciliation` receipts newer than the plan receipt, taking the newest one's `highest` (reuse `unreadable_line_may_be` for the refusal rule). Level = `rounds` when the count `< held_rounds`, else `after_p1` when the newest highest is `P1`, else `later`.
- [ ] **Step 4: Run** the file and `bash scripts/overnight/tests/run-all.sh`; expected all PASS.
- [ ] **Step 5: Commit** `review: the pin sets reviewer effort by area tier and the chain's last finding`, with `since` set to the commit time (amend the value before commit with `date +%s`).

### Task 4: The dispatcher applies the effort and records it

**Files:**
- Modify: `scripts/codex-dispatch.sh` (default `COMPANION` at line 128 to `$(repo root)/tools/codex-companion/scripts/codex-companion.mjs`; after `PIN_LENSES` at line 285, when `KIND` is a section kind, `EFFORT="$(python3 "$PIN_TOOL" --effort "$KIND" --todo "$TODO_PATH" --section-n "$SECTION" --manifest "$REVIEWS_DIR/manifest.jsonl" --paths-file0 "$PATHS_FILE")"`; on a non-zero exit log `[codex-dispatch] effort unknown (<reason>); the Codex config value applies` to the transcript and leave `EFFORT` empty; line 383 becomes `node "$COMPANION" adversarial-review ${EFFORT:+--effort "$EFFORT"} -- "$PROMPT"`; the receipt at line 427 gains `"opt:effort=${EFFORT:-}"`; line 456 stays as is (an unmarked direct call has no section))
- Modify: `scripts/overnight/review-broker-codex-dispatch.sh:180` (same default companion path)
- Test: `scripts/overnight/tests/test_review_pin.py` (the existing dispatcher tests there drive `codex-dispatch.sh` with a fake `CODEX_COMPANION_PATH`; extend the fake to append its argv to a file)

- [ ] **Step 1: Failing tests:**
  - `test_dispatch_passes_pinned_effort`: a fresh round on a deep-tier change; the fake's argv contains `--effort high` before `--`; the receipt line has `"effort": "high"`.
  - `test_dispatch_without_table_passes_no_effort`: pin without the table; argv has no `--effort`; receipt has no `effort` key.
  - `test_dispatch_effort_refusal_falls_back`: an unreadable newer manifest line; the review still runs, argv has no `--effort`, the transcript holds `effort unknown`.
  - `test_default_companion_is_vendored`: with `CODEX_COMPANION_PATH` unset, the resolved path (print it via the script's existing dry or debug hook, or assert by grep on the script text) is `tools/codex-companion/scripts/codex-companion.mjs`.
- [ ] **Step 2: Run**; expected FAIL.
- [ ] **Step 3: Implement** as in Files.
- [ ] **Step 4: Run** the file, `bash scripts/overnight/tests/run-all.sh` and `bash scripts/lint.sh`; expected PASS (Check 12 still sees single-quoted prompts).
- [ ] **Step 5: One real pass:** Task 6's first round receipt carries `"effort": "medium"` (section 70 touches only standard and risky paths, `src/libs/PROVENANCE.md` makes it risky) and its transcript shows the reviewer answered. Check it when that round returns.
- [ ] **Step 6: Commit** `review: the dispatcher passes the pinned effort and writes it on the receipt`.

### Task 5: Record, docs and the threat-model prompt rule

**Files:**
- Modify (through Bash, see Global Constraints): `todo/00-infrastructure/TODO-08-automation-hardening.md`: new `## 70. Reviewer effort follows the area and the chain` as the LAST section body, with its Implementation Order row (Depends On `§2`), items: pin table, dispatcher, vendored companion, upstream PR, measurement on 2026-12-07; a reciprocal XREF line under section 2 saying effort moved from the Codex config to the pin on 2026-11-23. Then `bash scripts/todo-graph/build-and-validate.sh --keep-cache` and `python3 scripts/todo-plan.py --sync`.
- Modify: `CLAUDE.md` (Codex Invocation Policy paragraph: effort is the operator's pin, applied by the dispatcher; Claude never passes or chooses it; the Codex config value is the fallback), `docs/infrastructure/codex-invocation-policy.md` (same, plus a line on the vendored companion), `.claude/skills/three-pass-review.md` (one line: effort is set by the pin; and one rule: a pass on a heuristic gate states the gate's threat model in its prompt, citing the spec's Related fix)
- Modify: the docs page that covers TODO-08 (find it with `grep -rl "covers=todo/00-infrastructure/TODO-08" docs`): bump `reviewed=`.

- [ ] **Step 1: Edit** the files above.
- [ ] **Step 2: Verify** `bash scripts/lint.sh` and `python3 scripts/site/build.py --check`; expected clean. `python3 scripts/todo-plan.py --check`; expected clean.
- [ ] **Step 3: Commit** `todo, docs: reviewer effort is the pin's, by area and finding` with the spec and this plan.

### Task 6: Review, ship, upstream

Order: Task 5's TODO filing and this task's plan pass run FIRST, before Task 1's code, because the pin reviews the plan before the build.

- [ ] **Step 1: Astra review** of section 70 through `.claude/skills/three-pass-review.md`: the plan pass on the spec and this plan before any code, then round, performance and completion on the candidate, fixing every finding with a test, to a clean wave or the cap. The vendored tree's unmodified commit is named in the prompt as imported code to read whole.
- [ ] **Step 2: Stamp** section 70 when the wave is clean.
- [ ] **Step 3: Push** when the primary tree has no unpushed commits: `git -C ../impossible-os-repair push origin HEAD:main` with `run_in_background: true`, then `python3 scripts/overnight/ci-wait.py .` until green.
- [ ] **Step 4: Upstream PR** to `openai/codex-plugin-cc` from a fork: the patch to `plugins/codex/scripts/codex-companion.mjs` and the three tests added to `tests/runtime.test.mjs`, title `adversarial-review: accept --effort like task`. Record the PR URL in `tools/codex-companion/PATCHES.md` and section 70.
- [ ] **Step 5: Clean up** nothing: `../impossible-os-repair` stays until the push is green, then `git worktree remove ../impossible-os-repair`.
