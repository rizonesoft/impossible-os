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
   - **Flat section numbering only:** `## 1.`, `## 2.`, `## 3.` -- the only numbered headings are top-level `## N. Title`. **Never** use sub-numbered section labels inside a section:
     - Forbidden: `### N.M` (e.g. `### 17.1`), bold pseudo-headings like `**17.1 Foo**` / `**1.2 Bar**`, or any `N.M` split that breaks the checklist into sub-chapters.
     - Required: under each `## N.` use **one continuous** `- [ ]` checklist (nested bullets under a single `- [ ]` line are OK for detail). Optional blocks only: a one-line intro, optional callouts (`> [!NOTE]` / WARNING / TIP), then all `- [ ]` work items with `- [ ] Commit:` as the **last** checklist line, then `**Test checkpoint:**` (plus its lines) as the **final** block in the section so acceptance criteria close the section. If you need grouping, fold the words into the bullet text (e.g. "App Paths: open HKLM key...") -- do not add `17.1` / `17.2` labels.
     - If validation finds `### N.M` or `**N.M ...**`, **remove** those headings and merge into a single list; never "fix" by renaming to bold `**N.M**`.
   - **`Depends On` column must use `§` prefix:** `§1, §3` not bare `1, 3`.
   - **Every section needs a `- [ ] Commit:` item.** Flag sections missing one.
   - **OS Comparison table (content + layout):** Required five-column header: `⭐ | Feature | 🪟 Win11 | 🐧 Linux | 🚀 Impossible OS`. Keep each cell to **status emoji or glyph + at most five words**. **Preserve source column alignment:** do not delete trailing spaces inside cells or shrink columns to shorten lines. If you change any cell, re-pad **every row** in that table (including the separator row) so pipes line up in the raw markdown -- separator cells must be `-` repeated to each column width (minimum 3 hyphens per column). A soft **~150 character** line limit per table row applies to **wording inside cells** only (about **50% wider** than the old ~100 guidance -- padded tables are allowed to use that headroom). Meet it by shortening labels or splitting notes, never by stripping padding. **Do not add** `<!-- Sources: ... -->` URL dumps; **delete** any legacy line matching that pattern if found (URLs belong in the gap-analysis chat report or PR text, not tracked TODOs).
7. **Win11/Linux parity and competitive edge scan (do not skip).**
   - **Parity gaps:** For each feature where Win11 AND Linux show ✅ but Impossible OS shows ⬜ or is missing: flag it. Verify it's covered by a section, another TODO (add XREF), or note as deferred with reason.
   - **Competitive edges:** Research what Win11 and Linux do poorly in this domain. For each opportunity, add a row marked ⭐ with ⬜ Planned and suggest a concrete section scope.
   - **Report findings explicitly** -- the user must see what was checked.
8. **XREF validation and loose ends.**
   - For each `→ XREF: TODO-XX §N`, confirm the target file exists and the referenced section number is present.
   - **Domain + TODO shorthand (mandatory for cross-domain):** When referencing another domain's TODO in Implementation Order `Depends On`, Inputs, prose, or History, use the compact notation from `.claude/skills/create-todo/implementation-order.md`: **same-domain** `TNN §N` (example `T17 §3`); **cross-domain** `DNN TNN §N` (example `D02 T19 §1` for `todo/02-kernel-core/TODO-19-...`). **`D02T19 §1`** (no space) is acceptable in tight table cells -- same meaning as `D02 T19 §1`. Domain digits are the folder prefix (`01-boot-platform` -> `01`, `02-kernel-core` -> `02`). Flag bare `TODO-19` or `TODO-02` alone when the owning domain is not obvious from context (ambiguous across 14 domains). A markdown path such as `02-kernel-core/TODO-19-foo.md` still satisfies clarity; add `D02T19 §N` when the file edits the `Depends On` column for scannability.
   - Check handoff boundaries: receiving TODOs should have matching Inputs or XREF entries.
   - **Loose end check:** For every "blocked by TODO-XX §N" or "deferred to TODO-XX" note:
     1. Verify the back-reference exists in the other TODO. Add one if missing.
     2. Deferred items must say WHERE (specific TODO + section) or WHY (condition to revisit). Never just "deferred."
   - **Internal §N check:** Verify every `§N` reference in prose/checklists points to the correct `## N.` heading. Flag self-references and semantic mismatches.
   - When findings involve external XREFs, patch the referenced TODO in the same run (add back-references, fix stale section numbers).
9. **Self-contained execution check.**
   - The TODO must be executable from §1 to the last section without being blocked by unimplemented external sections.
   - For each external `Depends On` entry in the Implementation Order:
     1. Must include specific section numbers: same-domain `T11 §1,§3`; cross-domain `D02 T19 §1` or compact `D02T19 §1` (never bare `TODO-19` for cross-folder deps).
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
13. **Test runner bat file.**
    - At the bottom of the `## Verification` section, ensure there is a line: `**Test runner:** `scripts\debug\run-<suite>-tests.bat` (SUITE=<cat>)`.
    - Determine the `TEST_CAT_*` category from the Unit Tests section (e.g., `TEST_CAT_MM` -> `mm`, `TEST_CAT_SCHED` -> `sched`).
    - Check if `scripts/debug/run-<suite>-tests.bat` exists on disk. If it does NOT exist, create it following the pattern in existing bat files (one-liner calling `run-qemu.ps1 -Accel whpx -TestOnly -TestSuite <suite>`).
    - Add the line to the Verification section if missing.
14. If section completion state seems wrong, defer to `/validate-todo-section` for deep code-truth verification.
15. **Update the History table** at the bottom of the TODO file (after Verification). If no `## History` section exists, create one. Append a row for this validation run:
    ```
    | Date | Action | Summary |
    ```
    Action = `validate`. Summary = one-line (sections checked, fixes applied, flags raised). Keep rows chronological, never delete old entries.

## Guardrails

- **Unicode en/em dash vs. prose:** Tracked files must not contain U+2013 (en) or U+2014 (em). **Do not** paste ASCII `--` as a typographic substitute in running text; it reads as minus, decrement, or noise (especially in C comments). **Rewrite** instead: colon, semicolon, parentheses, a short lead-in clause, or split into two sentences. Reserve `--` for meanings readers already expect (CLI flags in examples, markdown `---` rules, minus/range in formulas). See `CLAUDE.md` (No Unicode Dashes).
- **Never create or preserve N.M subnumbering** (`17.1`, `**3.2**`, `### 4.1`, etc.). One `## N.` section, one checklist stream.
- Do not implement code.
- Do not turn this into a formatting-only cleanup pass; structural clarity is the goal.
- Inputs path checks are existence-only -- do not read the referenced source files.
- **OS Comparison:** enforce the step 6 **content** rules (header shape, short cells). **Never** leave the table ragged in source (pipes misaligned) and **never** strip inter-column padding to satisfy line length. Re-pad the full table when any cell changes. Idempotent runs should not re-touch an already aligned table for "compaction" alone.
- Keep edits idempotent: running validation again on already-correct content should produce no further changes.
