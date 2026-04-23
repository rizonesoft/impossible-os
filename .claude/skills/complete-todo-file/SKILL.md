---
name: complete-todo-file
description: Finalize a TODO file whose `## N.` implementation sections are all shipped: sweeps for loose ends (stale XREFs, drifted test counts, unfilled placeholders, `[/]` items now runnable from the current host, `[ ]` "manual" items now reversibly-demonstrable), dispatches `/implement-unit-tests` on the TODO-level `## Unit Tests` section, executes every automatable command in the `## Verification` section (build, test runner, smoke test, grep checks), marks verified items `[x]` with captured evidence, flags manual-only items (bare-metal platforms, visual checks) as follow-ups, and commits the closure. Also handles the bare "any loose ends in TODO-XX?" / "close loose ends in TODO-XX" sweeps without requiring a full close-out -- the sweep step runs standalone when the user asks for it.
---

# Complete TODO File

> **External-Reviewer Contract:** This skill dispatches Codex indirectly via `/implement-unit-tests` (Codex test-coverage step) and through any automated test runs that invoke the Codex-review pipeline. Every finding from those dispatches goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot).

## Execution Discipline

> A closed TODO is a claim that what it promised is done. This skill owns the final two blocks of that claim -- **never fake a PASS you didn't run**.
> - **No PASS without execution.** Every `[x]` this skill writes into `## Verification` must come from a command that actually ran and returned the expected result. Paper completion is the exact failure mode this skill guards against.
> - **Stale info is a loose end.** A `2026-04-22 baseline 56 leaks advisory` line that the next-day commit closed to 0 leaks is silently lying about repo state. Refreshing those claims to current reality is non-negotiable; treat stale counts and unfilled `<commit>` placeholders the same as unchecked items.
> - **`[/]` items get a re-evaluation pass.** "User runs on rig" annotations that name TCG / KVM are runnable from this session today (KVM by default, TCG via `FORCE_TCG=1` in `scripts/test.sh`). Run them, capture the evidence, narrow the platform list. Only VBox + bare metal stay user-rig.
> - **`[ ]` "manual" items get a reversibility check.** A "break a syscall and watch the test catch it" demo is destructively-reversible: inject the break, run the suite, see the FAIL, revert, re-run clean. Demo it and close. A "visual frame inspection" or "boot on real hardware" item is not reversible from here -- those stay `[ ]`.
> - **Bare-metal items stay `[ ]`.** Lines that say "Verify on bare metal" / "Verify on VirtualBox" / "native Windows" are NOT automatable in this session. Mark them with a follow-up note, do not check them off.
> - **One TODO = one close-out commit.** Stage test-file wiring + Verification updates + sweep refreshes + any stamp edits together.
> - **If the TODO's `## N.` sections are NOT all `[x]`, the sweep step still runs in standalone mode.** Loose-end audit does not require all sections to be shipped; the close-out steps (unit-test dispatch + Verification execution + commit) do. A bare "any loose ends in TODO-XX?" question runs only steps 1-2, then 4 (sweep), then a chat report -- no commit.

## Use This Skill When

- A TODO's last numbered `## N.` section just shipped and the `## Unit Tests` + `## Verification` blocks still have unchecked items. Run the full workflow.
- The user says "close TODO-XX" / "finalize TODO-XX" / "run the verification on TODO-XX". Run the full workflow.
- The user says "any loose ends in TODO-XX?" / "close loose ends in TODO-XX" / "is TODO-XX clean?". Run the **sweep-only mode**: steps 1-2 (preconditions) + step 4 (loose-end + stale-info sweep) + close-out report. No unit-test dispatch, no Verification execution, no commit unless the sweep itself rewrote something. This mode also runs when a section is still `[/]` -- the sweep is allowed regardless of overall close-out readiness.
- Before graduating a TODO file from "active" to "doc-only reference" (after §5 work on long-lived TODOs). Run the full workflow.
- Do NOT use to finalize a single `## N.` section -- that is `/implement-todo-section` step 8 + step 17. This skill operates at the whole-TODO level.
- Do NOT use the FULL-CLOSE-OUT mode if any `## N.` section is still `[ ]` or `[/]` -- finish those first with `/implement-todo-section`. (Sweep-only mode does NOT have this restriction.)

## Workflow

1. **Read the TODO file.** Locate:
   - Implementation Order table status (every row must be `[x]`; flag any `[ ]` / `[/]` and stop).
   - `## Unit Tests` section (usually at end, before `## Verification`).
   - `## Verification` section.
   - All section bodies and their stamps (Notes / Verified / Accepted / Deferred / Quality reviewed) -- the loose-end sweep in step 4 audits these too.
   - Any forward-referenced `§N regression pack` items that live in later sections (skip; those ship under their own section).
2. **Precondition check.**
   - **Full close-out mode:** if Implementation Order has any non-`[x]` row, report it and STOP. The user runs `/implement-todo-section` on the remaining sections first.
   - **Sweep-only mode** (user asked "any loose ends in TODO-XX?" / "close loose ends in TODO-XX"): skip the precondition; proceed directly to step 4. Steps 3, 5-7, and the unit-test commit half of step 9 are skipped in sweep-only mode.
3. **Dispatch `/implement-unit-tests`** on the TODO's `## Unit Tests` section via the Skill tool, passing the TODO path as the argument. Let it run to completion: it creates the test file, registers it in `test_runner.c`, adds a `make test-<cat>` target if needed, runs the suite, and marks the Unit Tests items `[x]` with a short note (suite count + pass count). If the Unit Tests section says `> **Note:** No kernel test surface -- ...`, skip the dispatch and note the reason in the close-out report.
4. **Loose-end + stale-info sweep (MANDATORY in both modes).** BEFORE running automatable Verification items, audit the file for stale information and demonstrable open work. Each finding gets either fixed in place or added to the close-out report's "Loose ends" tally. Sweep targets:
   - **Stale `Accepted:` / `Deferred:` XREFs** (most common): grep the file for `Accepted:|Deferred:` lines, follow each XREF target, and check whether the named item / section is now `[x]`. If yes, edit the stamp in place to add a `(RESOLVED YYYY-MM-DD by §N commit <hash>)` annotation. Do NOT delete the original Deferred line -- it is audit history; the annotation closes the loop. **Fast path:** `python3 scripts/todo-graph/query.py deferred <todo-id>` lists every outbound stamp this TODO authored (targets to check for resolution) and `deferred-by <todo-id>` lists every inbound stamp pointing at it. Both resolve via frontmatter id / filename stem / slug; each row carries severity + kind + section + item_name, cutting the grep sweep to a single query.
   - **Stale numeric claims** in stamps + Verification + Notes: re-evaluate any `N tests passed` / `N suites` / `N/N PASS` / `N leaked` / `<N> assertions` / `<N> sentinels` claim more than ~7 days old (or older than the last `## N.` section commit, whichever is shorter). If the underlying suite is automatable from this host, run it and refresh the count. If the count drifted (typical: pass count grew 2-10% as new sections shipped), update in place. Re-running TCG-only checks needs `FORCE_TCG=1` if `scripts/test.sh` would auto-pick KVM.
   - **Unfilled placeholders**: grep for `<commit>`, `<hash>`, `<TBD>`, `<§N commit>`, `<DATE>`, `<commit-hash>`, `<X commits>`. Resolve each via `git log --grep '<keyword>' --format='%h %s'` (look for the section's commit) or current-date for date placeholders. Edit in place.
   - **`[/]` items with "user runs on rig" annotations**: re-check whether each platform mentioned (KVM / TCG / WHPX / VirtualBox / bare metal) is runnable from THIS session. KVM works in WSL2 today; TCG works in WSL2 via `FORCE_TCG=1`; WHPX requires native Windows; VBox requires VirtualBox; bare metal requires hardware. For each runnable platform, run the cited test, capture the evidence (count + leak result + commit-anchored date), and narrow the platform list in the `[/]` body. If every still-pending platform is unrunnable here, leave `[/]` with the annotation tightened to name only the actually-pending platforms.
   - **`[ ]` "manual demonstration" items**: re-evaluate reversibility. A "break X and watch the test catch it" item is destructively-reversible: inject the break (clearly tagged with a `DELIBERATE BREAK FOR <ITEM> -- REVERT` comment so a stray `git stash` is easy to spot), run the suite, capture the FAIL output verbatim, revert the change in the SAME session, re-run clean, capture the PASS output. Mark `[x]` with both outputs cited. A "visual frame inspection" / "boot on real hardware" / "native Windows behavior" item is NOT reversibly-demonstrable here -- those stay `[ ]`. An "add a new test binary works" item is provable by historical evidence: if N existing binaries already followed the documented add-path, cite the commit hashes that landed them and mark `[x]`.
   - **Stale "advisory until X" or "deferred until X ships" prose** in Verification or Notes: if X has shipped, replace the advisory text with the post-shipment reading (the actual measured number, the actual closing date, the actual commit hash).
   - **Deletes vs annotations**: stale `Accepted:` / `Deferred:` lines get **annotations** (RESOLVED ... by §N commit ...), not deletions. Stale numeric claims in `Notes:` / `Verified:` body text get **in-place rewrites** to the current number. The audit trail differs by line type: stamps are append-only history; Notes are the current-state summary.
5. **Parse `## Verification`** line-by-line. Classify each item:
   - **Automatable:** `bash scripts/build.sh[ clean]` / `make test-<cat>` / `bash scripts/test.sh QUIET=1` / `bash scripts/test-smoke.sh` / `grep -rn 'pattern' todo/` / `python3 ...` / `tail -1 build/build.log` / specific serial-log greps against existing capture files.
   - **Semi-automatable:** "POST16 codes 0xDDNN appear in correct order" -- run via `bash scripts/test-smoke.sh` + grep the stripped log at `build/smoke-test.stripped.log`.
   - **Manual only:** "Verify on QEMU WHPX (2 CPUs)" / "Verify on VirtualBox" / "Verify on bare metal" / "visual frame inspection" / any item requiring native Windows.
   - **Already closed by step 4 sweep**: skip; the demo + revert + re-run pass already captured the evidence.
6. **Execute the automatable items in order.** For each:
   - Capture the command + exit code + relevant stdout/stderr snippet (last 20 lines of relevant output is usually enough).
   - On PASS, mark the item `[x]` in the TODO file.
   - On FAIL, keep it `[ ]`, record the failure reason inline as a NOTE, and do NOT proceed to stamp the TODO as closed. The user resolves the failure before re-running this skill.
7. **For semi-automatable smoke-test items:** run `bash scripts/test-smoke.sh`, grep `build/smoke-test.stripped.log` for each expected POST16 code / klog marker, and mark individually.
8. **For manual-only items:** leave `[ ]`, append a `(manual -- run on <platform>)` tag so the user sees the remaining surface. Do NOT mark manual items `[x]` under any circumstance unless the step 4 sweep already demonstrated them via the destructively-reversible recipe.
9. **Generate a close-out report** (printed to the chat, not written into the TODO):
   - Implementation Order rows: N/N `[x]`.
   - **Loose ends closed (step 4):** named list grouped by category (stale XREFs annotated, stale counts refreshed, placeholders filled, `[/]` items narrowed or closed, `[ ]` manual items demonstrated). Each entry cites the file:line touched + the new value.
   - Unit Tests: `implement-unit-tests` result (suite count + pass count) OR "N/A (docs-only)" OR "skipped -- user gap-noted" OR "skipped -- sweep-only mode".
   - Verification items: M automated PASS, K automated FAIL (listed with reason), L manual-only (listed with platform).
   - Close-out state: **CLOSED** if all gates pass + 0 FAIL, **PARTIAL** if any FAIL exists, **MANUAL-PENDING** if only manual items remain, **SWEEP-ONLY** if the run was sweep-only mode.
10. **Commit and push** (only if state is CLOSED, MANUAL-PENDING, or SWEEP-ONLY-with-edits):
    - Full close-out commit message: `"close: <TODO file> -- unit tests wired; verification N/M automated PASS; L manual items pending"`.
    - Sweep-only commit message: `"todo: close <TODO file> loose ends (<short summary: what got rewritten>)"`. Skip the commit entirely if the sweep found nothing to rewrite (a clean-tree report still goes to the chat).
    - If state is PARTIAL, do NOT commit; report the failure and exit.
11. **Mark any `Commit:` checklist item inside Unit Tests / Verification as `[x]`** if the commit succeeded.

## Hook + Policy Integration

- **PreToolUse on `Skill` matcher includes this skill** in the completion-first reminder list (hook 8 in the Hook Routing Matrix). The reminder fires before the skill starts and pins the "no PASS without execution" rule.
- **Cross-TODO scope-gap awareness:** if the `## Verification` section references external TODO sections (e.g. "verify TODO-05 §14 stamps resolve"), this skill runs the lookup but does NOT author fixes in other TODO files. Out-of-scope findings land in the close-out report as `Accepted:` with an XREF pointer (the Accepted-XREF concreteness check from `review-todo-section` step 15 applies).
- **Inbound stamp sweep:** if this is the last section of a TODO marked `[x]` and other TODOs held `Accepted:` / `Deferred:` XREFs pointing at items this close-out just confirmed, grep `todo/` for those XREFs and report them as sweep candidates. Do NOT auto-rewrite inbound stamps; user confirms.

## Guardrails

- **No PASS without execution.** Every `[x]` this skill writes must come from a command that actually ran. Replay in chat the exact command + a pass-proof snippet.
- **No `[x]` on manual items.** Bare-metal / VirtualBox / native-Windows / visual-frame items stay `[ ]` with an explicit `(manual -- ...)` tag. The user ticks them off after running on the real platform. **Exception:** the step 4 sweep's destructively-reversible demo recipe (inject break -> run -> see FAIL -> revert -> re-run clean) closes "manual" items that are actually demonstrable from this host; cite both the FAIL and the post-revert PASS in the close-out report.
- **No section-level work in full-close-out mode.** If a `## N.` section has unshipped items, STOP -- user runs `/implement-todo-section` on that section first. (Sweep-only mode is exempt: the sweep is allowed even when sections are still `[/]`.)
- **No skipping `/implement-unit-tests`** in full-close-out mode. Even if the Unit Tests section looks partially wired, dispatch the skill; let it do the completeness check. Exception: the section explicitly documents no-kernel-test-surface in a `**Note:**` block, in which case capture that note in the report. Sweep-only mode skips the dispatch entirely.
- **Bat file / test category drift.** If the TODO's `## Unit Tests` names a runner bat that doesn't exist on disk, flag it -- `/implement-unit-tests` owns creating it per its workflow but this skill surfaces the gap. The right subdir per layer (split 2026-04-20):
  - Kernel `TEST_CAT_*` -> `scripts/debug/kernel/run-<suite>-tests.bat`
  - User-mode `test_*.exe` -> `scripts/debug/usermode/run-<binary>.bat`
  - Desktop UI -> `scripts/debug/desktop/run-<test>.bat`
  Per-category bats at the `scripts/debug/` root are stale (root is reserved for `run-all-tests.bat` only) -- flag those for relocation.
- **Commit discipline.** The close-out commit goes through the usual `Bash(git commit:*)` permission flow; the pre-commit hook 6 (section-commit GATE) does NOT fire here because this commit does not mix src + TODO edits -- it is a TODO-level close-out, not a section-implementation commit. The pre-commit lint hook still runs; TODO markdown passes as long as linters are clean. The sweep-only commit follows the same rules; the only difference is the message prefix (`todo: close ... loose ends` vs `close: ... -- unit tests wired ...`).
- **Sweep edits are scoped: stamps + Notes + Verification body, never structural blocks.** Implementation Order rows, OS Comparison narrative, and Inputs lists are read-only to the sweep. The sweep is allowed to:
  - **Annotate** stale `Accepted:` / `Deferred:` lines with `(RESOLVED YYYY-MM-DD by §N commit <hash>)` -- never delete the original line.
  - **Rewrite in place** stale numeric counts in Notes / Verified / Test runner / Verification body when the new measurement is from a real run captured in the same session.
  - **Resolve placeholders** like `<commit>`, `<§N commit>`, `<DATE>` to actual values from `git log` / current date.
  - **Narrow `[/]` body text** to drop already-closed platforms from the pending list (e.g. "TCG + VBox + bare metal" -> "VBox + bare metal" after running TCG via `FORCE_TCG=1`).
  - **Mark `[ ]` -> `[x]`** for manual-demonstration items the destructively-reversible recipe just demonstrated.
  The sweep is NOT allowed to: add new TODO items, modify Implementation Order rows, edit OS Comparison rows, change checklist body wording beyond the categories above, or touch other TODO files (cross-TODO XREF refresh stays out-of-scope; see step 4 stale-Accepted/Deferred handling -- the annotation lives in THIS file, not the cross-TODO target).
- **Never turn the full close-out into a broad file-wide cleanup either.** Outside the step 4 sweep's targeted edits, only the `## Unit Tests` and `## Verification` blocks get modified.

## Step 4 sweep recipes (concrete patterns from past runs)

These are battle-tested patterns from TODO-01 / TODO-02 / TODO-03 / TODO-04 sweeps (2026-04-23). Use them as templates; do not invent new categories.

- **Stale Accepted/Deferred XREF, target now closed.**
  Before: `> **Deferred:** [L] -ExtraArgs gap -> XREF: TODO-01 §8 (item: "..." at line 276)`
  After: `> **Deferred:** [L] -ExtraArgs gap (RESOLVED 2026-04-18 by §8 commit 11fa0d2a: -ExtraArgs threaded through every .ps1 + .bat scenario launcher).`

- **Stale test count claim.**
  Before: `> **Test runner:** ... | 1815 kernel + 16 user-mode PASS on KVM 2026-04-22`
  After: `> **Test runner:** ... | 2184 kernel + 16 user-mode PASS on KVM 2026-04-23 (exit=0); 2166 + 16 PASS on TCG via FORCE_TCG=1`

- **Unfilled placeholder.**
  Before: `> **Verified:** ... -> §4 closed 2026-04-20 \`<§4 commit>\``
  After: `> **Verified:** ... -> §4 closed 2026-04-20 \`11e816fb\``

- **`[/]` platform list narrowing after a runnable platform closes.**
  Before: `- [/] Run bash scripts/test.sh on QEMU WHPX (2 CPUs) + QEMU TCG + VirtualBox + bare metal ... TCG + VirtualBox + bare metal still pending; user runs those on their rig.`
  After: `- [/] Run bash scripts/test.sh on QEMU WHPX (2 CPUs) + QEMU TCG + VirtualBox + bare metal ... TCG 2026-04-23 (via new FORCE_TCG=1 override): 2166/2166 kernel + 16/16 user-mode PASS + 0 leaked. VirtualBox + bare metal still pending; user runs those on their rig.`

- **`[ ]` manual demo via destructively-reversible recipe.**
  Before: `- [ ] Break a syscall -> a user-mode test catches it (manual -- regression proof requires deliberately breaking a handler + re-running)`
  After: `- [x] Break a syscall -> a user-mode test catches it. Demonstrated 2026-04-23: injected ret = -1 in case SYS_GETPID, ran test.sh -> test_fastpath.exe FAIL (exit=4) + test_fastpath_fuzz.exe FAIL (exit=1) + wrapper exit=1. Reverted; re-run returned exit=0 + PASS: 2184 + 16.`

- **Stale advisory after dependency shipped.**
  Before: `L=56 is advisory per §8 contract -- existing tests surface previously-hidden leaks that need retrofitting in follow-up commits`
  After: `Observed after §9 leak-retrofit closed all 56 [LEAK] lines (2026-04-22): 1823/1823 PASS + 0 leaked on KVM; 1821/1821 PASS + 0 leaked on WHPX; 2166/2166 PASS + 0 leaked on TCG (2026-04-23). L=0 is now CI-gating (scripts/test.sh folds unannotated leaks into FAILED).`

- **Tracked hygiene residue (file flagged in V section but not removed).**
  Before: `NOTE: a tracked 0-byte .codex FILE remains from the 2026-04-06 cleanup -- not a parallel skill tree, so V8 is satisfied, but flagged as hygiene.`
  Action: `git rm .codex` + rewrite the Verification line: `The tracked 0-byte .codex FILE residue from the 2026-04-06 cleanup was removed 2026-04-23.`

## Relationship to other skills

- [`/implement-todo-section`](../implement-todo-section/SKILL.md) -- operates on a single `## N.` section with its own Unit Tests wiring (step 8) and Verification (step 17). This skill is the TODO-level counterpart that runs after all `## N.` sections ship.
- [`/implement-unit-tests`](../implement-unit-tests/SKILL.md) -- this skill dispatches it in step 3. Delegates the test-file creation + runner registration + bat-file wiring entirely.
- [`/review-todo-section`](../review-todo-section/SKILL.md) / [`/verify-todo-section`](../verify-todo-section/SKILL.md) -- section-level quality review and audit. This skill does NOT re-run them; sections that closed through review already carry their stamps.
- [`/validate-todo-file`](../validate-todo-file/SKILL.md) -- runs on TODO structure (numbering, XREFs, OS Comparison, Notes). This skill does not re-validate structure; if you want both, run `/validate-todo-file` before `/complete-todo-file`.
