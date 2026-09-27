<!-- docs: covers=todo/00-infrastructure/TODO-08-automation-hardening.md -->
# Triage Codex Finding -- The 3-Option Discipline

> Reference doctrine consumed by `superpowers:receiving-code-review`. Every
> Codex finding gets one of three classifications.

## Background

The receiving-code-review pattern offers three triage options:

1. **Fix** -- valid finding, fix at root cause.
2. **Reject** -- false positive, push back with code evidence.
3. **Accept-XREF** -- valid but out-of-scope, defer to a concrete `[ ]` item
   in another TODO with a domain-qualified XREF.

> **History note (2026-04-29 retraction):** an earlier draft of this doc
> added a 4th `Defer-to-tier` option keyed on a `RISK_TIER` declaration in
> the active skill flow. That doctrine was retracted because the tier
> classifier removed value from `implement-todo-section` enforcement -- a
> task declared `trivial` would skip Codex dispatches the section deserved.
> Every section now runs the full Codex pipeline (design + adversarial +
> consistency + perf + conditional re-adversarial + fix loop), so
> `Defer-to-tier` no longer has a meaningful place in triage. Findings
> that genuinely belong to lower-priority work get filed as `Accept-XREF`
> with a concrete owner item.

## The Three Options

| Option | When | Stamp shape |
|---|---|---|
| **Fix** | Valid finding within scope of the section being reviewed | (no stamp; just fixed) |
| **Reject** | False positive | (no stamp; rejection rationale lives in chat / commit message) |
| **Accept-XREF** | Valid, out-of-scope for THIS section, owned elsewhere | `Accepted: [<sev>] <finding> -> XREF: <domain/TODO-name (item: ...)>` |

## Decision Tree

```
Is the finding valid?
├── No  -> Reject (with code-evidence pushback)
└── Yes -> Is it in-scope for the section being reviewed?
           ├── Yes (in-scope) -> Fix
           └── No  -> Is the owner a concrete OTHER section/TODO with
                      a concrete `[ ]` checklist item?
                      ├── Yes -> Accept-XREF (cite the item by name/line)
                      └── No  -> File the concrete owner item NOW (Branch
                                 C/D scope-gap protocol), then Accept-XREF
                                 to it.
```

## Examples

### Fix (correct use)

A `review-todo-section` step 5 adversarial Codex flags an H finding: a
`mutex_lock()` race condition in the section's own implementation file.

→ **Fix.** Same-section issue; fix at root cause.

### Reject (correct use)

Codex adversarial flags an M finding: "function X is missing a NULL check".
Reading the cited line shows the caller of X already does an `assert(p !=
NULL)` 3 lines above the call site.

→ **Reject.** Code evidence: caller-side null check at `foo.c:42`.

### Accept-XREF (correct use)

Codex adversarial flags an M finding: "function signature inconsistent with
the equivalent in src/kernel/foo.c". The kernel side is owned by another
TODO section.

→ **Accept-XREF.** Open the kernel TODO, find the concrete item that owns
the signature drift, cite it by line number.

If no concrete owner item exists, file one BEFORE writing the stamp (per
the `Accepted-XREF concreteness check` in `review-todo-section/SKILL.md`
step 15 -- the same rule keeps `Accepted: TODO-XX §N` from becoming a
dead-end paper trail).

## Cross-references

- `review-todo-section` skill step 15 (Accepted-XREF concreteness check)
- `superpowers:receiving-code-review` (consumer; plugin-cache)
- `feedback_codex_review_workflow` memory
