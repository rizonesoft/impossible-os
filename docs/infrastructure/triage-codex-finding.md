# Triage Codex Finding -- The 4-Option Discipline

> Reference doctrine consumed by `superpowers:receiving-code-review`. Per the
> [review-pipeline right-sizing doctrine](../../todo/00-infrastructure/TODO-08-automation-hardening.md#22-review-pipeline-right-sizing-doctrine----risk_tier--bootstrap--tier-aware-triage--state-diagnosis--spiral-check)
> Codex findings get triaged against the active task's `RISK_TIER`, not
> adopted unconditionally.

## Background

Pre-doctrine, the receiving-code-review pattern offered three triage options:

1. **Fix** -- valid finding, fix at root cause.
2. **Reject** -- false positive, push back with code evidence.
3. **Accept-XREF** -- valid but out-of-scope, defer to a concrete `[ ]` item
   in another TODO with a domain-qualified XREF.

This worked when every task ran the full review pipeline. Once `RISK_TIER`
shipped, a fourth option became necessary: a `trivial` task should not
over-adopt findings whose severity-vs-risk ratio is calibrated for
`standard` or `critical` work.

## The Four Options

| Option | When | Stamp shape |
|---|---|---|
| **Fix** | Valid finding within scope, fits the active RISK_TIER | (no stamp; just fixed) |
| **Reject** | False positive | (no stamp; rejection rationale lives in chat / commit message) |
| **Accept-XREF** | Valid, out-of-scope for THIS section, owned elsewhere | `Accepted: [<sev>] <finding> -> XREF: <domain/TODO-name (item: ...)>` |
| **Defer-to-tier** (NEW) | Valid finding, lower priority than the active RISK_TIER warrants | `Deferred: [<sev>] <finding> (deferred-to-tier: <next-tier-task-XREF>)` |

## When to Defer-to-tier

The decision tree:

```
Is the finding valid?
├── No  -> Reject
└── Yes -> Is it in-scope (this section / this RISK_TIER)?
           ├── Yes (in-scope) -> Fix
           └── No  -> Is the owner a concrete OTHER section/TODO?
                      ├── Yes -> Accept-XREF
                      └── No  -> Is the finding's severity calibrated for
                                 a HIGHER tier than this task?
                                 ├── Yes -> Defer-to-tier
                                 └── No  -> Fix (re-evaluate; the fact
                                            that nobody owns it means
                                            you do)
```

## Examples

### Defer-to-tier (correct use)

A `RISK_TIER=trivial` task ships a 40-line host-side helper. Codex
adversarial flags an M finding: "race possible if two PostToolUse hooks
fire concurrently against the same state file". The codebase already
mitigates this for kernel-tier work via `fcntl.flock` in
`codex_review_completed.py::_record_stamp`; the host-side helper does not
hold load-bearing state.

→ **Defer-to-tier.** Stamp:
```
> **Deferred:** [M] Concurrent PostToolUse race on host-side helper state -- fcntl.flock pattern from kernel-tier work would close it (deferred-to-tier: standard tier follow-up; the helper does not yet hold load-bearing state)
```

### Fix (NOT defer-to-tier)

A `RISK_TIER=trivial` task adds a regex to a hook. Codex adversarial flags
an H finding: the regex has catastrophic backtracking on adversarial input.

→ **Fix.** Even at trivial tier, an H finding that affects the hook's own
correctness gets fixed. Defer-to-tier is for severity-vs-priority gaps,
not for severity-vs-tier overrides on the section's own correctness.

### Accept-XREF (NOT defer-to-tier)

Codex adversarial flags an M finding: "function signature inconsistent with
the equivalent in src/kernel/foo.c". The kernel side is owned by another
TODO section.

→ **Accept-XREF.** Defer-to-tier is for "lower-priority work that some
later tier will sweep up"; this finding has a known concrete owner now.

## Rule of thumb

`Defer-to-tier` is the option of LAST resort among the four. If you find
yourself reaching for it on more than ~10% of findings, the active task's
RISK_TIER is probably wrong (declared trivial when it should be standard).
Escalate the tier rather than deferring half the findings.

The stamp's `(deferred-to-tier: ...)` parenthetical is mandatory and
human-readable -- it should name the next task / sweep that will pick the
finding up, not just say "later".

## Cross-references

- [Review-pipeline right-sizing doctrine](../../todo/00-infrastructure/TODO-08-automation-hardening.md#22-review-pipeline-right-sizing-doctrine----risk_tier--bootstrap--tier-aware-triage--state-diagnosis--spiral-check) (parent)
- `superpowers:receiving-code-review` (consumer; plugin-cache)
- `feedback_codex_review_workflow` memory
