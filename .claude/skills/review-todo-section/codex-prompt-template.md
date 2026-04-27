# Codex Prompt Template -- review-todo-section step 8

> **Owner:** TODO-08-automation-hardening §5. Read `.claude/state/README.md`
> for the `last-review-stamps.json` schema this template feeds.

This file is a **fixed template** for the step-8 dispatches. The skill prose
forbids prepending the agent's own analysis: pasting the implementor's
narrative into the reviewer's prompt biases the reviewer toward agreement
([CONSENSAGENT, ACL-2025](https://aclanthology.org/2025.findings-acl.1141/)).
The template names files, sections, and adversarial angles only.

## Why this template exists

1. **Context isolation.** The reviewer must read the code cold, not through
   the implementor's framing.
2. **Dispatch attribution.** The leading `[review-kind: <kind>]` marker lets
   the §3 PostToolUse hook (`codex_review_completed.py`) record this dispatch
   to `last-review-stamps.json`. Without the marker, the bash invocation
   `codex-companion.mjs adversarial-review "..."` is identical across all
   three dispatches and the §4 commit gate cannot tell them apart.
3. **TODO path attribution.** The first `todo/<domain>/TODO-XX-<slug>.md`
   match in the prompt is recorded as the dispatch target. Multiple TODO
   paths in one prompt -> ambiguous, first wins, stderr WARN.

## Forbidden preambles

The §3 hook scans the first ~10 non-blank lines for these patterns and
emits a stderr WARN naming the matched line. WARN-only (false positives
on legitimate context lines are too easy); the goal is education.

- `^\s*I (built|wrote|implemented|made|added|created|fixed)\b`
- `^\s*Summary of changes`
- `^\s*What I built`
- `^\s*My (implementation|approach|fix|change)`
- `^\s*Here (is|'s) what I `

If the WARN fires, rewrite the prompt to use the template below. The
prompt should describe the SCOPE of review (files, sections, angles),
not the agent's narrative about the work.

## Three dispatch templates

Replace `<TODO_PATH>`, `<SECTION_NUM>`, and `<file:line ...>` placeholders
with concrete values. Keep the leading `[review-kind: ...]` marker exactly.
Section numbers are formatted `§N` (note: the `§` glyph may be unsafe in
some terminal contexts; the marker regex tolerates `section N` as well).

### Template: adversarial (step 5)

```
[review-kind: adversarial]
Target: <TODO_PATH> §<SECTION_NUM>
Files in scope: <comma-separated paths>

Adversarial review angles (mandatory; cover ALL):
- integer overflow / signed-unsigned narrowing
- buffer overread / off-by-one / unbounded copy
- NULL / uninitialized / use-after-free
- SMP races: missing locks, lock-order inversions, atomicity gaps
- ABI mismatch: struct layout, calling convention, error-code semantics
- resource leaks on error paths
- bounds on untrusted input (user-mode, disk, network)

For each finding: file:line, severity (Critical/High/Medium/Low),
specific evidence, and proposed fix. Do NOT propose stylistic changes.
Do NOT propose alternative architectures unless the current one has
a concrete defect.
```

### Template: consistency (step 8a)

```
[review-kind: consistency]
Target: <TODO_PATH> §<SECTION_NUM>
Files in scope: <comma-separated paths>

Consistency audit angles (mandatory; cover ALL):
- struct layout: byte-for-byte parity across kernel + bootloader mirrors
- constants: defined once, no silent duplication with drift potential
- API contracts: signature + error codes match consumer expectations
- ABI schemas: NVRAM variable name + GUID + attrs + size match producer/consumer
- SSDT row <-> function <-> registration table consistency
- Win32 vs NT semantics: the right surface for this layer
- narrowing / truncation / padding: same pattern across cross-file changes

For each finding: cite both files at file:line, name the divergence,
and propose a single canonical source. Do NOT propose changes that
require touching code outside the listed files.
```

### Template: perf (step 8b)

```
[review-kind: perf]
Target: <TODO_PATH> §<SECTION_NUM>
Files in scope: <comma-separated paths>

Performance audit angles (mandatory; cover ALL):
- allocations in hot paths / ISR contexts (kmalloc / pmm_alloc_contiguous calls)
- O(n^2) or worse on unbounded inputs
- spinlock hold times spanning I/O or serial writes
- byte-at-a-time operations that should be memcpy / memset
- branch-misprediction hazards on hot paths
- cache-line sharing across per-CPU state (false sharing)
- inline-asm barriers placed conservatively vs required
- memory copies on registered-handler paths (syscall / fault)

For each finding: file:line, observed asymptotic / hold-time / per-call
cost, AND a concrete remediation (specific algorithm or data-structure
change). Do NOT propose micro-optimizations under 10 cycles unless they
are on a documented hot path.
```

## What goes OUTSIDE the prompt

The agent's analysis, justifications, "what I built", "I think this
works", and per-finding triage notes do NOT go into the Codex prompt.
They go into:

- The agent's own conversation message AFTER receiving the Codex output.
- The TODO section's Notes block (per the brevity rules in
  `feedback_todo_notes_brevity`).
- The commit message body for per-finding adoption details.

The `superpowers:receiving-code-review` skill is the canonical surface
for triaging Codex findings; see CLAUDE.md "Mandatory Skill Triggers".
