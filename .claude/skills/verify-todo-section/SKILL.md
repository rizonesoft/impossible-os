---
name: verify-todo-section
description: Audit-mode wrapper over /review-todo-section. Runs the same quality pipeline (evidence, Codex adversarial, fixes, build, stamps) but in read-only / downgrade-only mode -- never marks new items [x], only downgrades [x] -> [/] / [ ] on regression evidence. Use to audit already-committed sections ("is §N still correct?"), re-verify after upstream changes, or confirm a section before graduating a TODO from active to documentation.
---

# Verify TODO Section

> **External-Reviewer Contract:** This skill dispatches Codex as part of its workflow. Every finding goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

## Execution Discipline

> The TODO section IS the contract being verified. This is the audit mode of [`review-todo-section`](../review-todo-section/SKILL.md) -- same workflow, different stance.
>
> - **Follow the contract, don't improvise.** The `[x]` items define what to verify. Do not widen.
> - **Downgrade-only.** Verify-mode NEVER marks `[ ]` or `[/]` items as `[x]`. It ONLY downgrades `[x]` -> `[/]` or `[ ]` when evidence shows regression. When in doubt, downgrade.
> - **Review pipeline is mandatory.** Codex adversarial + fixes + build + stamp gate (review-todo-section Phases 1-4) run unchanged. Audit-mode is a STANCE, not a shortcut.
> - **One section = one (optional) commit.** If verification finds clean, commit only the stamp. If verification finds fixes under audit-mode allowances (below), commit them together with `verify:` prefix. If verification finds regressions, downgrade and commit WITHOUT a stamp.

## Use This Skill When

- A section is marked `[x]` and you want to confirm it is still truly complete (upstream refactor, dependency churn, or time has passed).
- Before graduating a TODO file from active to documentation.
- The user asks "is §N really done?" / "re-verify §N" / "audit §N".
- Do NOT use when the section has never been implemented (all `[ ]`): invoke `/implement-todo-section` instead.
- Do NOT use for freshly-implemented code: that is exactly what `/review-todo-section` is for (and `/implement-todo-section` step 20 auto-runs it).

## Workflow

1. **Invoke [`/review-todo-section`](../review-todo-section/SKILL.md) and walk its 17-step pipeline with the audit-mode overrides in the next section.** The review skill owns the mechanics (evidence mapping, Codex dispatches, stamps, commit boundary); this skill owns the stance and the narrow fix-what-allowances.
2. **Apply the audit-mode overrides at every step where they bite.** The overrides are not optional; they are what makes verify-mode safe.
3. **On completion, stamp or downgrade** per the rules below.

## Audit-Mode Overrides (vs review-todo-section default)

| Concern                        | review default                                            | verify override                                                                                                              |
| ------------------------------ | --------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------- |
| Promoting checklist items      | May mark newly-finished work `[x]` if evidence supports.  | **NEVER** mark `[ ]` or `[/]` as `[x]`. New work belongs in `/implement-todo-section`. Items not already `[x]` are out-of-scope. |
| Fixing issues found            | Fix all valid Critical/High/Medium Codex findings.        | Fix ONLY under Branch A of [scope-gap-protocol.md](../implement-todo-section/scope-gap-protocol.md): under ~1000 lines, routine, same subsystem. Anything bigger: downgrade affected `[x]` and flag in the stamp's `Deferred:` line. |
| Adding missing tests           | Create new test files / categories as needed.             | ONE narrow allowance: if a single assertion for an already-existing function is missing, add it. Anything larger (new test file, new `TEST_CAT_*`, new suite) is a downgrade trigger, not a fix. |
| Adding missing checklist items | May append follow-up `[ ]` items under step 10 "Update TODO section". | Do NOT append new items. If verify reveals missing work, downgrade the related `[x]` items to `[/]` and let `/implement-todo-section` own the follow-up. |
| Stamp on regression            | Stamps even if fixes landed.                              | If ANY item was downgraded, do **NOT** add a stamp. Commit with `verify: ... downgraded <items>` and stop.                    |
| `- [ ] Commit:` checklist line | Implement-time; already `[x]` when verify runs.           | Never touch. Verify never marks the section's own `Commit:` item.                                                             |
| Commit message prefix          | `"review: <TODO> §N -- <summary>"`                         | `"verify: <TODO> §N -- verified clean"` (PASS) / `"verify: ... -- <fixes>"` (PASS-with-audit-fixes) / `"verify: ... -- downgraded <items>"` (FAIL, no stamp) |
| Scope-gap audit (step 10 / Branch A) | Implement the gap if small; file owner if large.     | **STATUS_NOT_IMPLEMENTED policy:** standalone stubs under Branch A get implemented; significant-infrastructure stubs get downgraded with `Accepted:` XREF to a new or existing owner section (never swept under the rug). |
| Stale POST16 / tautological tests | Remove with a note.                                    | Remove with a note in the verify commit. Behaviour identical.                                                                |
| Deep-analysis pass             | Optional; run on user request or hot-path code.           | Same -- opt-in to `codex-consistency-audit`, `codex-perf-review` when the section is hot-path or the user asked for thorough.                                                        |

## Stamp Format

Identical to review-todo-section's stamp section. Same `Verified:` / `Accepted:` / `Deferred:` / `Quality reviewed:` blockquote lines with the same field rules (severity tags, item-parenthetical, evidence-token vocabulary, `<details>` escape hatch). The one difference is the trigger:

- **PASS (clean):** add or update the stamp in place.
- **PASS-with-audit-fixes:** add or update the stamp; the `Quality reviewed:` line's `Nx fixed` count records the audit-mode fixes.
- **FAIL (any downgrade):** do NOT add a stamp. Downgrades alone are the audit trail.

Re-verify replaces existing stamp lines in place; never duplicate.

## What This Skill Does NOT Do

- Does NOT mark `[ ]` or `[/]` items as `[x]`.
- Does NOT append new checklist items.
- Does NOT create new test files or test categories.
- Does NOT widen scope beyond Branch A audit-mode fixes.
- Does NOT modify the section's `- [x] Commit:` checklist line.
- Does NOT replace `/review-todo-section`. Freshly-implemented code goes through review, not verify.

## Section-Commit Gate Note

The `.claude/hooks/section_commit_gate.py` hook fires on the same staged-diff signature whether the source is implement-mode or verify-mode (source change + Implementation Order `[x]` flip in the same commit). For verify-mode, an audit-fix commit that DOWNGRADES `[x]` to `[/]` or `[ ]` will NOT match the signature (the regex looks for flips TO `[x]`, not from); the gate stays out of the way for downgrade commits. An audit-fix commit that promotes a fixed item to `[x]` (rare; verify default is downgrade-only) WILL match the signature and require evidence. Same opt-out (`SKIP_REVIEW_HOOK=1` + `SKIP_REVIEW_HOOK_REASON="..."`) applies.

## Guardrails

- **Conservative downgrade only.** Never widen scope. The two places verify-mode may touch code are Branch A scope-gap fixes and the one-assertion test gap-fill.
- **The HARD GATE is review-todo-section's, inherited whole.** Codex adversarial + fixes + build + loose-end sweep run under audit-mode overrides; review-skip is not an option.
- Do not weaken tests to make findings go away.
- For SSDT sections, verify the full service-number / Nt-function / registration / master-table chain before downgrading or accepting status. Auto-triggers `/audit-ssdt`.
- Do not turn verify into a broad file-wide cleanup pass.
- Never introduce `N.M` subnumbering.
- Apply `superpowers:receiving-code-review` discipline to every Codex finding, same as review.

## Relationship to review-todo-section

Verify inherits review's entire 17-step workflow, HARD GATE, and stamp format. The two skills differ only in STANCE: review is oriented toward just-implemented code and may fix-and-promote; verify is oriented toward already-committed code and may only fix-and-downgrade-or-accept. The overrides table above is the complete delta.

Editing review-todo-section's workflow propagates automatically to verify; editing this file should only be needed to adjust the audit-mode overrides or stance prose.

## Additional Resources

- [scope-gap-protocol.md](../implement-todo-section/scope-gap-protocol.md) -- Branch A/B/C/D decision tree; Branch A is the verify-mode fix allowance.
- [build-evidence.md](../implement-todo-section/build-evidence.md) -- shared build-confirmation rules.

## TODO-08 §10 step-state telemetry

The TODO-08 §10 step-observer hook records each step on its real tool call; the §10 step-block hook BLOCKs commit if any required terminal step's evidence is missing. There is no "I did it inline" shortcut -- the hook does not see narration. The hook fires on `Bash(git commit:*)` and `Skill(review-todo-section)`. Required terminal steps for this skill are listed in `.claude/hooks/skill_step_map.py`. Opt-out (legitimate revert / stamp-only flows): `SKIP_SKILL_STEP_BLOCK=1 SKIP_SKILL_STEP_BLOCK_REASON="<text >= 12 chars>"`.
