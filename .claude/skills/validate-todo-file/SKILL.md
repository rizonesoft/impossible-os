---
name: validate-todo-file
description: Validate a TODO file for structural completeness, Implementation Order accuracy, XREF continuity, cross-TODO scope overlap, Inputs path existence, parity gaps, false completeness, ownership, and test coverage. Use when reviewing a TODO after creation, major edits, or during implementation tracking.
---

# Validate TODO File

## Execution Discipline

> This is a structure-and-readiness pass, not a formatting-only sweep. The TODO must be executable, ownership-complete, and resistant to paper completion.
> - **Completion-first.** A valid TODO does not stop at the happy path if adjacent work is obviously required for the feature to feel real.
> - **No ownerless deferrals.** If the plan defers work, the exact owner section or TODO must already exist.
> - **Parity is the floor.** A strong TODO also leaves room for competitive refinement where the domain warrants it.

## Use This Skill When

- A TODO file was just created or significantly edited and needs structural validation.
- The `/todo-pipeline` invokes this as stages 1 and 3 (before and after gap analysis).
- The user asks "validate TODO-XX" or "check this TODO file" (not a specific section).
- Before starting implementation on a TODO to ensure it's structurally sound.
- Do NOT use for single-section audit -- the code-truth pass lives as step 17 of `/implement-todo-section`, or use `/review-todo-section` for a full quality review with Codex dispatches.

## Workflow

0. **Dispatch the evidence mapper first:** `Agent(subagent_type="todo-validation-mapper")`
   on the target TODO. It returns the structural evidence map (sections vs
   Implementation Order table, per-section stamp census, XREF-target existence
   with quoted lines, Inputs-path existence, checklist counts) in a throwaway
   context. Use it to scope steps 4-6; verify every load-bearing finding at
   file:line before acting on it (trust contract). The mapper supplements, it
   does not replace, the main-session read in step 1 -- verdicts stay here.
1. Read the full TODO file, not just a selected section.
2. Clean up formatting throughout the file.
   - Run Grep for continuation lines: `rg '^ {2,}[a-zA-Z`'"'"'"]' <file>` -- fix all matches before proceeding.
   - Remove blank lines between consecutive list items in the same group.
   - **Join hard-wrapped mid-sentence line breaks with the SCRIPT, not by hand:** `python3 scripts/todo-reflow.py --diff <file>` to review, then `--write` to apply. `todo/` prose is ONE PARAGRAPH PER LINE, wrapped by the reader's editor; a fill column makes a section inconsistent with the file around it and turns every later edit into a reflow. This step previously read "join hard-wrapped mid-sentence line breaks" as model-driven prose, which is exactly how TODO-21 sections 19-20 shipped wrapped at 120 columns (measured 2026-07-28: 102 chars/line against 209 for the rest of the file). The script is deterministic and idempotent, REFUSES any reflow that would alter content (whitespace-normalised comparison; file left untouched on mismatch), and leaves bullets, tables, headings, fenced and indented code, two-line notes, and runs of complete sentences alone. Do not hand-reflow a long TODO -- that is the operation where a paragraph silently loses a clause. Authoring-time prevention is the `todo_wrap_reminder` hook (warn-only).
   - No blank lines inside a single callout block. No blank lines between table rows.
   - Remove orphaned bold labels (standalone `**Title**` lines that aren't `**Test checkpoint:**` etc.).
   - Max 1 consecutive blank line anywhere. Preserve blank lines between headings, groups, and `---` separators.
   - **Section separators (`---`) -- mandatory between every numbered section + before standard caps.** The repo convention (see TODO-01..TODO-05 for reference shape) is a `---` separator on its own line + blank line + the next `## ` heading. Required between every adjacent pair of `## N.` numbered sections, AND between the last numbered section and `## OS Comparison`, AND (when present) before `## Unit Tests` / `## Verification` / `## History`. NOT required between the top-of-file callouts (`## Inputs` / `## Outcome` / `## Implementation Order` group) since those are part of the introductory cap. The validator must check that **every adjacent numbered-section transition has a `---` between the prior section's `**Test checkpoint:**` block and the next `## N.` heading**. Insert missing separators with a blank line above + below: `\n---\n\n## N. ...`. Do not add separators inside a section body or between bullets in the same section. Idempotency: a re-run on a fully-separated file produces zero edits. The pre-OS-Comparison `---` is the most commonly-correct position; missing inter-section separators are the typical drift the gap-analysis or template-derived TODOs leave behind.
3. **Remove model tags from section headings.**
   - Strip `` `[Opus]` `` and `` `[Sonnet]` `` postfixes from all `## N. Title` headings.
4. Anchor-check the Inputs section.
   - For each file path listed in Inputs, use Glob to confirm it exists on disk.
   - Flag broken anchors; suggest correct path or note as planned.
5. Domain overlap scan.
    - Read the domain `INDEX.md` and other TODO files in the same folder.
    - Grep for deliverable names and feature keywords to detect scope overlap.
    - Scan for duplicate XREFs where two TODOs both *implement* the same section.
    - **Programmatic integrity gate:** run `bash scripts/todo-graph/build-and-validate.sh --keep-cache` to catch cross-TODO XREF drift the structural sweep cannot. It runs the FULL check set the validator prints as `N/M checks passed` (stale XREF, dangling `§N`, orphan IO row, dep cycle, missing bat, status-transition mismatch, `$schema` unreachable, duplicate id, duplicate resolver key, in-file anchor -- do not hardcode the count here, it grows; the tool's own tally is the source of truth). Any failure there is a real violation that blocks ready-to-implement; fix it or downgrade the affected section before finishing validation. (Owned by the todo-graph CI gate; local validator run mirrors the PR-time check.)
6. Validate the lean TODO structure.
    - Check required sections, numbering, checklist shape, references, and exit criteria.
    - Verify the TODO has a clear completion boundary:
      - `> [!IMPORTANT] Current state:` callout exists after the Goal paragraph.
      - Numbered sections do not stop at paper completion when obvious adjacent work still needs to be named.
      - Deferred work is filed into a concrete owner section or TODO, not left as vague prose.
    - **Flat section numbering only:** `## 1.`, `## 2.`, `## 3.` -- the only numbered headings are top-level `## N. Title`. **Never** use sub-numbered section labels inside a section:
     - Forbidden: `### N.M` (e.g. `### 17.1`), bold pseudo-headings like `**17.1 Foo**` / `**1.2 Bar**`, or any `N.M` split that breaks the checklist into sub-chapters.
     - Required: under each `## N.` use **one continuous** `- [ ]` checklist (nested bullets under a single `- [ ]` line are OK for detail). Optional blocks only: a one-line intro, optional callouts (`> [!NOTE]` / WARNING / TIP), then all `- [ ]` work items with `- [ ] Commit:` as the **last** checklist line, then `**Test checkpoint:**` (plus its lines) as the **final** block in the section so acceptance criteria close the section. If you need grouping, fold the words into the bullet text (e.g. "App Paths: open HKLM key...") -- do not add `17.1` / `17.2` labels.
     - If validation finds `### N.M` or `**N.M ...**`, **remove** those headings and merge into a single list; never "fix" by renaming to bold `**N.M**`.
   - **`Depends On` column must use `§` prefix:** `§1, §3` not bare `1, 3`.
   - **Every section needs a `- [ ] Commit:` item.** Flag sections missing one.
   - **Every section with `[x]` or `[/]` Implementation Order status needs a `> **Notes:**` block.** It sits between the `> **Test runner:**` line and the `> **Verified:**` stamp and carries 3-6 bullets summarizing what shipped (see `implement-todo-section` step 10 for the canonical shape: what shipped, how it runs/integrates, downstream effects, canonical doc pointer, scope boundary). Flag completed sections missing the block -- it is the human-readable counterpart to the machine-readable stamps. Sections still `[ ]` (not shipped yet) MUST NOT have a Notes block.
   - **OS Comparison table (content + layout):** **Placement:** The whole `## OS Comparison` section is the `## OS Comparison` heading, the five-column markdown table, and any summary lines or callouts directly under it until the next top-level `## ` heading. It must sit **after** the last numbered implementation section (`## N. Title`) and **immediately before** `## Unit Tests`, matching `.claude/skills/create-todo/todo-template.md`. If it appears earlier (for example right after `## Implementation Order` or `## Outcome` while `## 1.` or higher still appears later, or between two implementation sections), **move** the entire block to the canonical spot. An optional standalone `---` line immediately above `## OS Comparison` is allowed. If `## Unit Tests` is missing, place OS Comparison immediately before `## Verification` and follow step 12 for a skeleton. **After any move:** re-pad the full table so columns align (same padding rules as after cell edits). **Required five-column header:** `⭐ | Feature | 🪟 Win11 | 🐧 Linux | 🚀 Impossible OS`. **Prefer** platform cells that start with a status emoji or glyph and add a **short** status phrase when that stays accurate. **Do not** shorten, merge, or rewrite rows **only** to make the table more compact: preserve distinct Feature labels and parity nuance. If wording is vague or wrong, fix meaning first; optional tightening is secondary. **Too terse:** If a Feature label or platform cell is so short that parity is unclear (emoji-only, generic yes/no, filler duplicated across rows), **expand** with specific accurate wording or a scoped pointer (`§N`, `TNN §N`, or a concrete subsystem or policy name). Prefer one clear clause over many vague stubs. If you cannot state the comparison truthfully without research, flag it in the step 7 report and leave an honest partial/unknown marker rather than empty micro-copy. **Row length cap (200 chars, enforced):** Every raw markdown table row (from leading `|` through trailing `|`, including intentional inter-column spaces) MUST be **200 characters or fewer**. When a row exceeds 200, compact it: drop filler words (`usually`, `often`, `standard`, `emerging` when ⚠️ already implies it, `in practice`, `by repo`, `available`), shorten header column names (`🪟 Win11 projects` -> `🪟 Win11`), or replace long prose with a `§N` / `TNN §N` pointer to the section that owns the detail. Also shorten the section header line (`| ⭐ | Feature | ...`) and the separator line to match the new column widths. The cap is firm; **exceptions require an inline HTML comment** on the line above the row: `<!-- row-length-exempt: <one-line reason> -->` (e.g. a long verbatim API path that cannot be truncated without losing the fact). Without that sentinel, rows over 200 are a validation finding. **Never** hollow cells, merge distinct comparisons, or strip alignment padding **only** to satisfy the cap; if compaction would lose a fact, use a `§N` pointer or add the exemption sentinel with a real reason. **Preserve source column alignment:** do not delete trailing spaces inside cells or shrink columns to shorten lines. If you change any cell, re-pad **every row** in that table (including the separator row) so pipes line up in the raw markdown -- separator cells must be `-` repeated to each column width (minimum 3 hyphens per column). **Do not add** `<!-- Sources: ... -->` URL dumps; **delete** any legacy line matching that pattern if found (URLs belong in the gap-analysis chat report or PR text, not tracked TODOs).
7. **Win11/Linux parity and competitive edge scan (do not skip).**
    - **Parity gaps:** For each feature where Win11 AND Linux show ✅ but Impossible OS shows ⬜ or is missing: flag it. Verify it's covered by a section, another TODO (add XREF), or note as deferred with reason.
    - **Competitive edges:** Research what Win11 and Linux do poorly in this domain. For each opportunity, add a row marked ⭐ with ⬜ Planned and suggest a concrete section scope.
    - **False-completeness scan:** For each section, ask whether literal implementation would still leave an obvious adjacent piece missing: failure-path handling, registrations, exports, tests, docs, reciprocal XREFs, or the next capability needed for the feature to feel real.
    - **Ownership scan:** every deferred/blocked note must point at a concrete owner section or TODO item, not just vague future intent.
    - **Report findings explicitly** -- the user must see what was checked.
8. **XREF validation and loose ends.**
   - For each `→ XREF: TODO-XX §N`, confirm the target file exists and the referenced section number is present.
   - **Domain + TODO shorthand (mandatory for cross-domain):** When referencing another domain's TODO in Implementation Order `Depends On`, Inputs, prose, or History, use the compact notation from `.claude/skills/create-todo/implementation-order.md`: **same-domain** `TNN §N` (example `T17 §3`); **cross-domain** `DNN TNN §N` (example `D02 T19 §1` for `todo/02-kernel-core/TODO-19-...`). **`D02T19 §1`** (no space) is acceptable in tight table cells -- same meaning as `D02 T19 §1`. Domain digits are the folder prefix (`01-boot-platform` -> `01`, `02-kernel-core` -> `02`). Flag bare `TODO-19` or `TODO-02` alone when the owning domain is not obvious from context (ambiguous across 14 domains). A markdown path such as `02-kernel-core/TODO-19-foo.md` still satisfies clarity; add `D02T19 §N` when the file edits the `Depends On` column for scannability.
   - Check handoff boundaries: receiving TODOs should have matching Inputs or XREF entries.
    - **Loose end check:** For every "blocked by TODO-XX §N" or "deferred to TODO-XX" note:
      1. Verify the back-reference exists in the other TODO. Add one if missing.
      2. Deferred items must say WHERE (specific TODO + section) or WHY (condition to revisit). Never just "deferred."
      3. If the referenced owner section exists but has no concrete checklist item for the gap, create or request one. A section title alone is not an owner.
    - **Internal §N check:** Verify every `§N` reference in prose/checklists points to the correct `## N.` heading. Flag self-references and semantic mismatches.
    - When findings involve external XREFs, patch the referenced TODO in the same run (add back-references, fix stale section numbers).
9. **Self-contained execution check.**
    - The TODO must be executable from §1 to the last section without being blocked by unimplemented external sections.
    - The TODO must also be **credibly complete**: implementing it literally should not leave obvious adjacent work unplanned or ownerless.
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
    - If a section defers a feature on purpose, ensure the Unit Tests plan makes room for the later real assertion or a temporary `TEST_PENDING` pattern rather than silently dropping test ownership.
13. **Test runner bat file (subdir-aware).**
    - At the bottom of the `## Verification` section, ensure there is a one-line compact stamp pointing at the **right subdir** for the test layer (the bat-runner subdirs split 2026-04-20: kernel-side TEST_CAT into `scripts/debug/kernel/`, user-mode `test_*.exe` into `scripts/debug/usermode/`, desktop UI into `scripts/debug/desktop/`):
        - **Kernel TEST_CAT_* suite:** `**Test runner:** `scripts\debug\kernel\run-<suite>-tests.bat` (SUITE=<cat>) | N suites, 0 failures`
        - **User-mode test binary:** `**Test runner:** `scripts\debug\usermode\run-<binary>.bat` (utest_filter=<binary>) | N suites, 0 failures`
        - **Desktop UI test:** `**Test runner:** `scripts\debug\desktop\run-<test>.bat` | N suites, 0 failures`
        - **No test surface:** `**Test runner:** N/A (<reason>) | validation: <how-verified>`
    - Determine the test layer from the Unit Tests section: `TEST_CAT_*` enum value -> kernel; `user/test/test_*.c` mention -> user-mode; `src/desktop/` test mention -> desktop. Then determine the suite/binary/test name accordingly (e.g., `TEST_CAT_MM` -> `mm`, `test_syscall.exe` -> `test_syscall`).
    - Check if the matching bat exists on disk in the right subdir. If it does NOT exist, create it following the pattern in existing bat files. The relative path `%~dp0..\..\machines\run-qemu.ps1` is correct for any of the three subdirs (one extra `..\` for the directory split).
    - **Aggregate runners (one per subdir):** if creating a NEW per-category bat, also confirm the matching `run-all-<layer>-tests.bat` aggregate exists in the same subdir (`kernel/run-all-kernel-tests.bat`, `usermode/run-all-usermode-tests.bat`, `desktop/run-all-desktop-tests.bat`). The root `scripts/debug/run-all-tests.bat` chains all three aggregates and uses `if exist` so missing per-layer aggregates are no-ops; create them only when at least one per-category bat exists in the subdir.
    - **Forbidden:** putting a per-category test bat at the `scripts/debug/` root. That location is reserved for `run-all-tests.bat` (the cross-layer aggregate) only; everything else lives in a kernel/usermode/desktop subdir.
    - Add the line to the Verification section if missing. Prefer the single-line pipe-separated format over splitting across `**Test runner:**` + `**Expected:**` blockquote lines.
14. If section completion state seems wrong, defer to `/review-todo-section` (full Codex + domain quality sweep) or to step 17 of `/implement-todo-section` (cheap code-truth reconciliation) depending on how deep the audit needs to go.

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

17. **Write the file-level Validated stamp (lifecycle marker).** When the structural pass is clean (all findings fixed, or none found), write/refresh the file-preamble stamp immediately under the H1 and above `> **Goal:**`:
    `> **Validated:** YYYY-MM-DD | validate-todo-file clean (structure / IO table / XREF / test wiring)`.
    One stamp line, not a History block; refresh the date on each clean run. If a structural blocker cannot be resolved this pass, do NOT write the stamp -- report the blocker instead. (An absent stamp correctly tells the overnight sequencer's Stage 0 to re-run validation next pass; the oracle `.claude/hooks/sequencer_triage.py` reads this stamp as half of `stages_1_2_done`.)

## Guardrails

- **Unicode en/em dash vs. prose:** Tracked files must not contain U+2013 (en) or U+2014 (em). **Do not** paste ASCII `--` as a typographic substitute in running text; it reads as minus, decrement, or noise (especially in C comments). **Rewrite** instead: colon, semicolon, parentheses, a short lead-in clause, or split into two sentences. Reserve `--` for meanings readers already expect (CLI flags in examples, markdown `---` rules, minus/range in formulas). See `CLAUDE.md` (No Unicode Dashes).
- **Never create or preserve N.M subnumbering** (`17.1`, `**3.2**`, `### 4.1`, etc.). One `## N.` section, one checklist stream.
- Do not implement code.
- Do not turn this into a formatting-only cleanup pass; structural clarity is the goal.
- Inputs path checks are existence-only -- do not read the referenced source files.
- Do not leave paper completion in place. If a section would still feel obviously incomplete after literal implementation, add the missing adjacent work or file the owner explicitly.
- Do not leave deferred work ownerless. "Deferred" without a concrete owner section or TODO item is invalid.
- **OS Comparison:** enforce step 6 **placement** (after last `## N.`, before `## Unit Tests`), **layout** rules (header shape, aligned pipes, padding preserved), and the **soft** ~200 character raw-row target (meaning and padding beat the number). Relocating a misplaced `## OS Comparison` block is required structural work. **Never** leave the table ragged in source (pipes misaligned) and **never** strip inter-column padding to satisfy line length. Re-pad the full table when any cell changes or after moving the block. Idempotent runs should not re-touch an already valid table for cosmetic shortening or "compaction" alone. Underspecified cells (compact but meaningless) are a **content** defect: expand with accurate detail, add a pointer, or flag for research in the parity report.
- Keep edits idempotent: running validation again on already-correct content should produce no further changes.
