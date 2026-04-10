---
name: validate-todo-file
description: Validate a TODO file for structural completeness, Implementation Order accuracy, XREF continuity, cross-TODO scope overlap, Inputs path existence, parity gaps, and test coverage. Use when reviewing a TODO after creation, major edits, or during implementation tracking.
---

# Validate TODO File

## Use This Skill When

- A TODO file was just created or significantly edited and needs structural validation.
- The `/todo-pipeline` invokes this as stages 1 and 3 (before and after gap analysis).
- The user asks "validate TODO-XX" or "check this TODO file" (not a specific section).
- Before starting implementation on a TODO to ensure it's structurally sound.
- Do NOT use for single-section validation -- use `/validate-todo-section` instead.

## Workflow

1. Read the full TODO file, not just a selected section.
2. Clean up formatting throughout the file.
   - Run Grep for continuation lines: `rg '^ {2,}[a-zA-Z`'"'"'"]' <file>` -- fix all matches before proceeding.
   - Remove blank lines between consecutive list items in the same group.
   - Join hard-wrapped mid-sentence line breaks in prose, callouts, and XREF notes.
   - No blank lines inside a single callout block. No blank lines between table rows.
   - Remove orphaned bold labels (standalone `**Title**` lines that aren't `**Test checkpoint:**` etc.).
   - Max 1 consecutive blank line anywhere. Preserve blank lines between headings, groups, and `---` separators.
3. **Remove model tags from section headings.**
   - Strip `` `[Opus]` `` and `` `[Sonnet]` `` postfixes from all `## N. Title` headings.
4. Anchor-check the Inputs section.
   - For each file path listed in Inputs, use Glob to confirm it exists on disk.
   - Flag broken anchors; suggest correct path or note as planned.
5. Domain overlap scan.
   - Read the domain `INDEX.md` and other TODO files in the same folder.
   - Grep for deliverable names and feature keywords to detect scope overlap.
   - Scan for duplicate XREFs where two TODOs both *implement* the same section.
6. Validate the lean TODO structure.
   - Check required sections, numbering, checklist shape, references, and exit criteria.
   - **Flat section numbering only:** `## 1.`, `## 2.`, `## 3.` -- no sub-sections (`### 1.1`). Flatten and renumber if found.
   - **`Depends On` column must use `§` prefix:** `§1, §3` not bare `1, 3`.
   - **Every section needs a `- [ ] Commit:` item.** Flag sections missing one.
   - **Compact the OS Comparison table:** Header: `⭐ | Feature | 🪟 Win11 | 🐧 Linux | 🚀 Impossible OS`. Cells: emoji + max 5 words. ~100 char rows max.
7. **Win11/Linux parity and competitive edge scan (do not skip).**
   - **Parity gaps:** For each feature where Win11 AND Linux show ✅ but Impossible OS shows ⬜ or is missing: flag it. Verify it's covered by a section, another TODO (add XREF), or note as deferred with reason.
   - **Competitive edges:** Research what Win11 and Linux do poorly in this domain. For each opportunity, add a row marked ⭐ with ⬜ Planned and suggest a concrete section scope.
   - **Report findings explicitly** -- the user must see what was checked.
8. **XREF validation and loose ends.**
   - For each `→ XREF: TODO-XX §N`, confirm the target file exists and the referenced section number is present.
   - Check handoff boundaries: receiving TODOs should have matching Inputs or XREF entries.
   - **Loose end check:** For every "blocked by TODO-XX §N" or "deferred to TODO-XX" note:
     1. Verify the back-reference exists in the other TODO. Add one if missing.
     2. Deferred items must say WHERE (specific TODO + section) or WHY (condition to revisit). Never just "deferred."
   - **Internal §N check:** Verify every `§N` reference in prose/checklists points to the correct `## N.` heading. Flag self-references and semantic mismatches.
   - When findings involve external XREFs, patch the referenced TODO in the same run (add back-references, fix stale section numbers).
9. **Self-contained execution check.**
   - The TODO must be executable from §1 to the last section without being blocked by unimplemented external sections.
   - For each external `Depends On` entry in the Implementation Order:
     1. Must include specific section numbers (`T11 §1,§3`), not bare TODO numbers.
     2. If the external section is `[x]`: no action needed.
     3. If `[ ]`: flag as **blocked**. Suggest adding a minimal local prerequisite section or note the blocker.
   - Flag any `Depends On` referencing something that doesn't exist yet.
10. **Prerequisite code audit.**
    - For each function, type, or API referenced in checklist items, Grep to check if it exists in the codebase.
    - If it doesn't exist AND is not created by a prior section: flag as missing prerequisite.
    - If created by a prior section: verify the section order puts creator before consumer.
11. **Platform coverage check (bare metal first).**
    - Every TODO touching hardware, interrupts, page tables, timers, or drivers must list platforms per section.
    - Required: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
    - Flag sections that only mention VMs: add "Verify on bare metal -- VM behavior differs."
12. **Test enforcement.**
    - Every section MUST have a `**Test checkpoint:**` block with concrete pass/fail criteria (serial strings, POST codes, observable behavior). Flag vague checkpoints ("should work").
    - Every TODO file MUST have a `## Unit Tests` section wired into `test_runner_init()`.
    - Each test case must be a specific assertion (function + expected value). Flag vague "test that X works" items.
    - If the Unit Tests section is missing entirely, flag it and draft a skeleton.
13. If section completion state seems wrong, defer to `/validate-todo-section` for deep code-truth verification.

## Guardrails

- Do not implement code.
- Do not turn this into a formatting-only cleanup pass; structural clarity is the goal.
- Inputs path checks are existence-only -- do not read the referenced source files.
- Preserve unrelated formatting; but the OS Comparison table MUST be compacted per step 6 every time -- this is NOT "reformatting", it is enforcing the project standard (~100 char rows, emoji + max 5 words per cell).
- Keep edits idempotent: running validation again on already-correct content should produce no further changes.
