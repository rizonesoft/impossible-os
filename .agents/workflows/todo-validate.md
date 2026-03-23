---
description: Review a TODO file for gaps, inconsistencies, and missing features — then fix everything
---

# Validate a TODO File

Review a TODO file for completeness, accuracy, and competitive features. Fix all issues found:
gaps, inconsistencies, broken references, misaligned tables, missing codebase enhancements,
and missing Impossible OS exclusive features.

## When to Run

- After creating or heavily editing a TODO file
- When a TODO file hasn't been reviewed in a while
- When new codebase capabilities should be reflected in the TODO
- As a periodic quality check before implementation begins

## Input

A single TODO file path, e.g., `todo/060-Hardware-Drivers/TODO-060-PCI.md`

## Steps

### 1. Read the full TODO file

Use `view_file` to read the entire TODO file. Note:
- Total section count and numbering gaps
- Which sections are `[x]` (complete), `[/]` (in progress), `[ ]` (pending)
- Whether the file has all required structural elements (see step 2)

### 2. Verify structural completeness

Every TODO file **must** contain these elements. Flag any that are missing:

| Element                            | Required? | Check                                                                 |
| ---------------------------------- | --------- | --------------------------------------------------------------------- |
| `# Title` (H1)                    | ✅ Yes     | Matches file naming: `NNN.NN-ShortName — Full Title`                  |
| Goal blockquote                    | ✅ Yes     | `> **Goal:**` with 2–4 sentence description                          |
| Critical notices (CAUTION/WARNING) | If needed | Hardware constraints, memory rules, spec references                   |
| Dependency Graph (mermaid)         | ✅ Yes     | `graph TD` showing section dependencies                               |
| Phase-by-Phase Implementation table| ✅ Yes     | Uses ⭐/💎 icons, Phase numbers, Status (✅/⬜/⚠️)                    |
| Phase notes (NOTE/TIP blocks)      | ✅ Yes     | Explains each phase's purpose                                         |
| Implementation sections            | ✅ Yes     | `## N.` / `### N.M` with Prompt or Verification block                |
| Checkbox items per section         | ✅ Yes     | `- [ ]` or `- [x]` items in every section                            |
| Commit items per section           | ✅ Yes     | Last checkbox: `- [ ] Commit: "scope: description"`                   |
| Priority Order table               | ✅ Yes     | 🔴P0 / 🟠P1 / 🟡P2 / 🟢P3 / 🔵P4 with all sections listed          |
| OS Comparison table                | ✅ Yes     | 🪟/🐧/🚀 columns with ✅/⬜/⚠️ status emojis                        |
| OS Comparison summary blockquote   | ✅ Yes     | `> **After P0+P1 items:** ...` progression summary                    |

For any missing element, **add it** following the format in `todo-create.md`.

### 3. Check section-level consistency

For **each** section in the TODO:

#### 3a. Prompt / Verification block
- **Pending sections (`[ ]`):** Must have a `**Prompt:**` block with clear implementation instructions
  ending with the standard closing: "After completing all items, mark every item as `[x]`, update this
  prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"scope: description"`.
  Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and
  important information to MCP memory."
- **Completed sections (`[x]`):** Must have a `**Prompt:**` block rewritten as a verification prompt
  (starts with "This section is marked complete. Verify..." or "✅ VERIFICATION —"), OR a clear
  implementation note explaining what was done

#### 3b. Checkbox items
- Every section must have at least 2 checkbox items
- The **last** checkbox must be a commit instruction: `- [ ] Commit: "scope: description"`
- Completed items must be `[x]`, not `[ ]` — if the prompt says "done" but items are unchecked, fix them
- No orphaned sub-items (`  - [ ]` without a parent `- [ ]`)

#### 3c. Cross-references (XREF)
- Every `→ XREF:` line must reference a real section in a real TODO file
- Verify the referenced file exists: `todo/<folder>/TODO-NNN*.md`
- Verify the referenced section (`§N.M`) exists within that file
- Fix or remove stale XREFs

#### 3d. Duplicate content
- Check for sections that appear twice (copy-paste errors)
- Check for repeated IMPORTANT/WARNING blocks within the same section
- If duplicates found, keep the more complete version and remove the other

### 4. Verify reference accuracy

#### 4a. Internal references
- All `§N.M` references within the file must point to existing sections
- All mermaid graph node labels must match actual section titles
- Dependency graph edges must reflect actual prerequisite relationships

#### 4b. External file references
- Links to other TODO files: verify the file exists at the referenced path
- Links to spec files (`specs/...`): verify the spec file exists
- Links to source files (`src/...`, `include/...`): verify the file exists via `find_by_name`
- Links to docs (`docs/...`): verify the doc exists

#### 4c. Spec section references
- If the TODO cites spec sections (e.g., "per VirtIO 1.2 §5.2.3"), spot-check that the spec
  file has that section and the content matches what the TODO claims

#### 4d. Commit hash references
- If a completed section references a commit hash (e.g., `(32a5f8f)`), verify it exists:
  `git log --oneline | grep <hash>`

### 5. Examine the codebase for enhancements

Use Srclight to understand the current implementation state and identify improvements:

```
1. mcp_srclight_codebase_map()          — orient yourself
2. mcp_srclight_search_symbols(query)   — find symbols related to the TODO
3. mcp_srclight_get_symbol(name)        — read implementations
4. mcp_srclight_get_callers(name)       — understand usage patterns
5. mcp_srclight_symbols_in_file(path)   — audit file structure
```

For each major TODO section:

1. **Find the relevant source files** — search for symbols, structs, and functions mentioned in the TODO
2. **Compare actual code vs TODO claims** — if the TODO says "implement X" but X already exists, update
   the TODO to mark it as done or note the existing implementation
3. **Identify enhancement opportunities:**
   - Functions that exist but are incomplete (error handling, edge cases)
   - Missing features that the code structure makes easy to add
   - Performance improvements based on current architecture
   - New APIs or hook points that could be exposed
4. **Add new checkbox items** for discovered enhancements — mark them with ⭐ if they're
   Impossible OS exclusives

### 6. Research and add competitive features

**This step is mandatory.** Search for features that could make Impossible OS superior:

1. **Search the web** for how Windows 11 and Linux handle this component
2. **Identify pain points** in both OSes:
   - Missing features users complain about
   - Poor error messages or recovery
   - No telemetry or observability
   - Slow or inefficient implementations
3. **Design Impossible OS exclusives** that address these gaps
4. **Add new sections** for exclusive features (mark with 🚀 and ⭐)
5. **Update the Priority Order table** to include new features
6. **Update the OS Comparison table** to include new features

**Common exclusive feature categories:**

| Category               | Pattern                                                        |
| ---------------------- | -------------------------------------------------------------- |
| Adaptive behavior      | Dynamic mode switching based on workload metrics               |
| Telemetry              | ns-resolution latency histograms, IOPS counters exposed in GUI |
| Priority mapping       | Win32 I/O priority → hardware queue QoS                        |
| Predictive             | Driver-level prefetch, request merging, pattern detection      |
| Multi-device           | Driver-level striping, mirroring, or aggregation               |
| Hot operations         | Proactive auto-resize, graceful surprise removal               |
| Registry integration   | Tunable parameters via `HKLM\SYSTEM\Drivers\...`              |
| GUI integration        | Real-time dashboards in Disk Manager / Device Manager          |
| Self-healing           | Auto-recovery from errors without user intervention            |
| Developer experience   | Better debug output, hardware report, diagnostic commands      |

### 7. Align all table columns

Pad table cells so columns line up in raw markdown. Apply to **every** table in the file.

**Before:**
```markdown
| Feature | Status | Description |
|---|---|---|
| PCI scan | ✅ | Done |
| MSI-X | ⬜ | Not started |
```

**After:**
```markdown
| Feature  | Status | Description |
| -------- | ------ | ----------- |
| PCI scan | ✅     | Done        |
| MSI-X    | ⬜     | Not started |
```

Rules:
- Pad all cells to the width of the widest cell in that column
- Use consistent separator row style (dashes padded to column width)
- Keep table width ≤ 120 characters — split wide tables if needed

### 8. Fix the OS Comparison table format

The OS Comparison table **must** include a `⭐` icon column as the **first column** to
distinguish spec-defined features (💎) from Impossible OS exclusives (⭐):

```markdown
## OS Comparison

| ⭐ | Feature                          | 🪟 Windows 11                     | 🐧 Linux                            | 🚀 Impossible OS                                |
| -- | -------------------------------- | --------------------------------- | ------------------------------------ | ----------------------------------------------- |
| 💎 | Basic feature                    | ✅ How Windows does it             | ✅ How Linux does it                  | ✅ Done — brief description                      |
| 💎 | Partially done feature           | ✅ Windows approach                | ✅ Linux approach                     | ⚠️ §N.M PN — what's missing                     |
| 💎 | Not yet started feature          | ✅ Windows approach                | ✅ Linux approach                     | ⬜ §N.M PN — brief plan                         |
| ⭐ | **Exclusive feature name**       | ❌ Short gap description           | ❌ Short gap description              | ⬜ §N.M PN — **competitive advantage** 🚀       |
```

**Icon meanings:**
- 💎 = Spec-defined / standard feature (Win32 parity, industry standard)
- ⭐ = Impossible OS exclusive (competitive advantage, neither Windows nor Linux has it)

**If the icon column is missing**, add it:
1. Insert `| ⭐ |` before the Feature column header
2. Insert `| -- |` in the separator row
3. For each row: insert `| 💎 |` for standard features, `| ⭐ |` for exclusive features
4. Exclusive features are identified by: bold text in Feature column, `🚀` suffix, or
   rows where both Windows and Linux show `❌`

**Required format rules:**
- Column headers: `⭐` | `Feature` | `🪟 Windows 11` | `🐧 Linux` | `🚀 Impossible OS`
- Status emojis: ✅ = done, ⬜ = not started, ⚠️ = partial
- Impossible OS column references section + priority: `§N.M PN`
- Exclusive features bolded with 🚀 suffix
- Summary blockquote after the table

**After the table, include a progression summary:**
```markdown
> **After P0+P1 items:** Impossible OS matches Windows and Linux feature-for-feature.
> **After P2–P3 exclusive features:** Exceeds both — [list advantages].
> **After P4 items:** Full spec parity with enterprise features.
```

### 9. Fix the Phase-by-Phase Implementation Order table

Verify the phase table uses the canonical format:

```markdown
### Phase-by-Phase Implementation Order

| Phase  | Sections                         | Depends On                    | Status |
| :----: | -------------------------------- | ----------------------------- | :----: |
| **0**  | Prerequisites (spec, code, etc.) | —                             |   ✅   |
| **1**  | §1.1 Foundation Section          | Phase 0                       |   ⬜   |
| **3**  | §5.1 Exclusive Feature           | Phase 2 (§2.1)                |   ⬜   |
```

> [!IMPORTANT]
> The Phase table does **not** have a `⭐/💎` icon column. That column is only for
> the OS Comparison table. If you find an icon column in a Phase table, remove it.

Verify:
- All sections appear in the phase table
- Dependencies are consistent with the mermaid dependency graph
- Status matches the actual checkbox state of each section
- New sections from step 6 are included

### 10. Final consistency sweep

After all changes:
- Re-read the file end-to-end
- Verify no broken markdown (unclosed code blocks, orphaned list items)
- Verify all table columns are still aligned after edits
- Verify section numbering is sequential (no gaps within a group, gaps between groups are OK)
- Verify the Priority Order table includes every section
- Verify the OS Comparison table includes every feature
- Verify the dependency graph includes every section

### 11. Format all tables

Run the `/todo-table-format` workflow on the TODO file to standardize all tables:
- Add ⭐/💎 column to OS Comparison and Priority Order tables
- Shorten Phase column values (`**3**` → `P3`)
- Align all table columns
- Do NOT commit — save only

### 12. Commit

If any fixes were made:

```bash
git add -A && git commit -m "todo: validate and enhance <TODO-file-name>

- Fixed N gaps/inconsistencies
- Added N exclusive features
- Aligned N tables
- <key changes summary>"
```

## Checklist

Before committing, verify:
- [ ] All required structural elements are present (step 2 table)
- [ ] Every section has a Prompt (pending) or Verification (done) block
- [ ] Every section has checkbox items with a commit instruction
- [ ] All cross-references (XREF) point to real sections in real files
- [ ] All file references (specs, source, docs) point to existing files
- [ ] Codebase enhancements are reflected (new items from Srclight analysis)
- [ ] Competitive features researched and added (🚀 exclusives)
- [ ] All table columns are aligned
- [ ] OS Comparison table uses canonical 🪟/🐧/🚀 format with ✅/⬜/⚠️
- [ ] Phase table uses canonical ⭐/💎 format with correct dependencies
- [ ] Priority Order table lists every section
- [ ] Summary blockquote after OS Comparison
- [ ] No duplicate sections or repeated content blocks
- [ ] Section numbering is consistent
- [ ] Mermaid dependency graph matches phase table
