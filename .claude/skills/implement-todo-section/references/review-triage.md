# Codex finding triage -- design and adversarial

Read on demand from `implement-todo-section` steps 4 and 13. The step text in SKILL.md carries the mandatory
action (dispatch, then receive every finding through `superpowers:receiving-code-review`); this file carries the
per-finding decision detail and the false-positive shapes measured in this repo.

## The rule that governs both dispatches

Every finding is verified at `file:line` before it is actioned, and lands in exactly one of Fix / Reject / Accept.
Never blind-implement; never performatively agree. Canonical contract:
[docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

## Design review (step 4) -- false-positive watch

Codex suggests APIs that don't fit the codebase pattern, misses constraints already enforced elsewhere, pushes
complexity already resolved differently, or recommends libraries that aren't available. Bulk-adopting design findings
without verification is the same blind-implementation failure mode the discipline was created to prevent.

For EACH design finding (Critical/High/Medium/Low):

- **Verify against existing code first.** Read the file:line Codex cites to confirm the situation actually matches
  Codex's claim. Check whether the suggested change is already partially or fully in place under a different name.
  Check whether a related constraint (CLAUDE.md rule, existing pattern in the file, reciprocal API in another module)
  already addresses the concern.
- **If valid:** adopt the design change. Note WHY in the implementation (one-line comment near the relevant code, or a
  "Codex design review caught X" line in the file's header docstring). When ready to mark the checklist item `[x]` at
  step 10, REWRITE the item wording to reflect the adopted approach, not the original draft.
- **If wrong/misleading:** reject with concrete code evidence (the suggested API already exists at file:line, the
  constraint Codex thinks is missing is enforced at file:line, the alternative Codex proposed conflicts with CLAUDE.md
  X). Do NOT silently drop -- name the rejection in chat so the user sees the design rationale.
- **If out of scope (genuine cross-section concern):** accept with domain-qualified XREF to a concrete `[ ]` item in
  the appropriate later section / TODO, same as adversarial-review's Accepted-XREF rule.

**Record in the TODO section's pre-stamp `> **Notes:**` block** (see step 10) using a single-line "Codex design review
adoptions in commit `<hash>`" reference -- detailed per-finding evidence belongs in the commit message, not in Notes
(the brevity rule from `feedback_todo_notes_brevity`). The stamp's `Codex Nx (design + adversarial + consistency +
perf)` counter at step 16 reflects the design dispatch when it happens.

## Adversarial review (step 13) -- false-positive watch

Codex reads code without runtime context and frequently flags "missing lock" when the caller already holds it, "race"
on paths single-threaded by construction, or "buffer overflow" on buffers static-asserted larger than the access.

Verify at file:line; fix valid Critical/High at the root cause; reject wrong findings with code evidence; accept
out-of-scope with domain-qualified XREF (not lazy deferral). An Accepted XREF must point at a concrete `[ ]` item --
see the Accepted-XREF concreteness check in [todo-bookkeeping.md](todo-bookkeeping.md).
