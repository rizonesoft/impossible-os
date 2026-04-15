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
   - **OS Comparison table (content + layout):** **Placement:** The whole `## OS Comparison` section is the `## OS Comparison` heading, the five-column markdown table, and any summary lines or callouts directly under it until the next top-level `## ` heading. It must sit **after** the last numbered implementation section (`## N. Title`) and **immediately before** `## Unit Tests`, matching `.claude/skills/create-todo/todo-template.md`. If it appears earlier (for example right after `## Implementation Order` or `## Outcome` while `## 1.` or higher still appears later, or between two implementation sections), **move** the entire block to the canonical spot. An optional standalone `---` line immediately above `## OS Comparison` is allowed. If `## Unit Tests` is missing, place OS Comparison immediately before `## Verification` and follow step 12 for a skeleton. **After any move:** re-pad the full table so columns align (same padding rules as after cell edits). **Required five-column header:** `⭐ | Feature | 🪟 Win11 | 🐧 Linux | 🚀 Impossible OS`. **Prefer** platform cells that start with a status emoji or glyph and add a **short** status phrase when that stays accurate. **Do not** shorten, merge, or rewrite rows **only** to make the table more compact: preserve distinct Feature labels and parity nuance. If wording is vague or wrong, fix meaning first; optional tightening is secondary. **Too terse:** If a Feature label or platform cell is so short that parity is unclear (emoji-only, generic yes/no, filler duplicated across rows), **expand** with specific accurate wording or a scoped pointer (`§N`, `TNN §N`, or a concrete subsystem or policy name). Prefer one clear clause over many vague stubs. If you cannot state the comparison truthfully without research, flag it in the step 7 report and leave an honest partial/unknown marker rather than empty micro-copy. **Soft row length:** Treat each raw markdown table row (from leading `|` through trailing content and intentional inter-column spaces) as a **soft** ~200 character guide: aim near or under when accuracy is unchanged. Rows may exceed it when detail is required. **Never** hollow cells, merge distinct comparisons, or strip alignment padding **only** to satisfy the number; when a row is long and redundant, prefer clearer phrasing or a `§N` / `TNN §N` pointer over rambling. **Preserve source column alignment:** do not delete trailing spaces inside cells or shrink columns to shorten lines. If you change any cell, re-pad **every row** in that table (including the separator row) so pipes line up in the raw markdown -- separator cells must be `-` repeated to each column width (minimum 3 hyphens per column). **Do not add** `<!-- Sources: ... -->` URL dumps; **delete** any legacy line matching that pattern if found (URLs belong in the gap-analysis chat report or PR text, not tracked TODOs).
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

15. **Code block compaction (MANDATORY).** Inline ` ```c ``` ` blocks are for SHOWING THE CONTRACT, not for pre-writing the implementation. A TODO is a plan; the header file and source file are the implementation. Duplicating struct definitions, enum tables, and function bodies in the TODO creates four problems: (a) the TODO drifts from the code, (b) the implementer copy-pastes from the TODO instead of designing the type fresh against current invariants, (c) the section bloats from ~10 lines to ~80, making the checklist unscannable, (d) future readers cannot tell the spec from a stale draft.

    **What's allowed:**
    - Single struct WITH `_Static_assert` lines (3-8 lines) showing an offset/size contract that crosses subsystems (e.g. assembly reads it; bootloader writes it).
    - 1-3 line snippet showing a critical signature or invariant the section establishes.
    - Wire format / on-disk format spec (the bytes ARE the contract).

    **What's NOT allowed (compact aggressively):**
    - Full struct definitions with field comments. Replace with a single bullet naming the struct + listing fields by name + total size: `Define PORT_MESSAGE (40 bytes) in include/kernel/ipc/alpc.h: TotalLength, DataLength, Type, DataInfoOffset, CLIENT_ID, MessageId, CallbackId. Reference: winternl.h.`
    - Enum / `#define` tables with more than 4 constants. Replace with a count + naming scheme + reference: `Define 10 ALPC_MSG_TYPE_* constants (REQUEST=1 ... CONNECTION_REQUEST=10) per Windows ALPC convention.`
    - Function bodies, even small ones. The implementer will write them.
    - Any block that re-states what `man 3 X` or MSDN already says verbatim.

    **Detection:** any ` ```c\n...``` ` block exceeding ~15 lines is presumed bloated. Compact to a bullet that names the type + fields/constants + size + external reference. The compacted form must preserve the same information density (every constant value, every field name, every size invariant) -- it just doesn't pre-write the C code.

    **Example transformation:**
    ```c
    typedef struct {
        uint32_t Flags;
        SECURITY_QUALITY_OF_SERVICE SecurityQos;
        uint64_t MaxMessageLength;
        ...
    } ALPC_PORT_ATTRIBUTES;
    ```
    becomes:
    `Define ALPC_PORT_ATTRIBUTES (8 fields per MSDN ALPC reference): Flags (uint32, ALPC_PORTFLG_*), SecurityQos (SECURITY_QUALITY_OF_SERVICE), MaxMessageLength/MemoryBandwidth/MaxPoolUsage/MaxSectionSize/MaxViewSize/MaxTotalSectionSize (uint64), DupObjectTypes (uint32 bitmask).`

    **Forbidden parenthesized N.M labels.** `- [ ] (1.1 PORT_MESSAGE header) Define ...` is the same anti-pattern as `### 1.1` in disguise -- a checklist item label that carves a section into sub-chapters. Strip the `(N.M Title)` prefix; the bullet itself names what is being defined.

    The PostToolUse hook on `todo/**/*.md` flags any new ` ```c ``` ` block over 15 lines AND any `(N.M Title)` parenthesized prefix in checklist items. The hook is a reminder, not a block; intentional spec-defining blocks (wire format, ABI contract assertions) opt out by adding `<!-- spec-block-ok: <one-line-reason> -->` immediately above the ` ``` `.

16. **Do NOT add a `## History` section or row.** The git log and per-section stamps already carry the audit trail; a History table just repeats the same dates with less detail and grows without bound. Leave existing History sections alone (don't delete prior entries) but do not append new rows.

## Guardrails

- **Unicode en/em dash vs. prose:** Tracked files must not contain U+2013 (en) or U+2014 (em). **Do not** paste ASCII `--` as a typographic substitute in running text; it reads as minus, decrement, or noise (especially in C comments). **Rewrite** instead: colon, semicolon, parentheses, a short lead-in clause, or split into two sentences. Reserve `--` for meanings readers already expect (CLI flags in examples, markdown `---` rules, minus/range in formulas). See `CLAUDE.md` (No Unicode Dashes).
- **Never create or preserve N.M subnumbering** (`17.1`, `**3.2**`, `### 4.1`, etc.). One `## N.` section, one checklist stream.
- Do not implement code.
- Do not turn this into a formatting-only cleanup pass; structural clarity is the goal.
- Inputs path checks are existence-only -- do not read the referenced source files.
- **OS Comparison:** enforce step 6 **placement** (after last `## N.`, before `## Unit Tests`), **layout** rules (header shape, aligned pipes, padding preserved), and the **soft** ~200 character raw-row target (meaning and padding beat the number). Relocating a misplaced `## OS Comparison` block is required structural work. **Never** leave the table ragged in source (pipes misaligned) and **never** strip inter-column padding to satisfy line length. Re-pad the full table when any cell changes or after moving the block. Idempotent runs should not re-touch an already valid table for cosmetic shortening or "compaction" alone. Underspecified cells (compact but meaningless) are a **content** defect: expand with accurate detail, add a pointer, or flag for research in the parity report.
- Keep edits idempotent: running validation again on already-correct content should produce no further changes.
