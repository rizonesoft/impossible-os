---
description: Add ⭐💎 column, shorten Phase to P format, and align columns in TODO file tables
---

# TODO Table Formatting

Standardize the tables in a TODO file to match the project's table format conventions.

## What This Fixes

1. **Missing ⭐💎 column** — Every row in Phase-by-Phase and OS Comparison tables must have a leading `⭐` or `💎` marker column
2. **Verbose Phase column** — Phase values like `**3**` or `Phase 3` should be shortened to `P3`
3. **Misaligned columns** — All table columns must be visually aligned with consistent padding
4. **Verbose Depends On references** — `Phase 3 (§5.1)` should be shortened to `P3 (§5.1)`

## Marker Rules

- **⭐** = Feature where Impossible OS is **superior** to both Windows and Linux (competitive advantage)
- **💎** = Standard/parity feature (matches what Windows and Linux already do)

Use ⭐ when the OS Comparison table shows ❌ or ⚠️ for both Windows and Linux on that feature. Use 💎 when both competitors have ✅.

## Steps

### 1. Identify the tables to fix

Look for these tables in the TODO file:
- `### Phase-by-Phase Implementation Order` — the roadmap table
- `## Priority Order` — the priority ranking table (already has ⭐ column usually)
- `## OS Comparison` — the feature comparison table

### 2. Fix the Phase-by-Phase Implementation Order table

**Before:**
```markdown
| Phase  | TODO File / Spec  | Sections      | What It Delivers | Depends On         | Status |
| :----: | ----------------- | ------------- | ---------------- | ------------------ | :----: |
| **0**  | `040.01`          | Block device  | blkdev_read()    | —                  |   ✅   |
| **3**  | `040.04-MBR.md`   | §5.1 Writer   | MBR write        | Phase 2 (§3.1)     |   ⬜   |
```

**After:**
```markdown
| ⭐ | P    | TODO File         | Sections      | What It Delivers | Depends On     | Status |
| -- | :--: | ----------------- | ------------- | ---------------- | -------------- | :----: |
| 💎 | P0   | `040.01`          | Block device  | blkdev_read()    | —              |   ✅   |
| 💎 | P3   | `040.04-MBR.md`   | §5.1 Writer   | MBR write        | P2 (§3.1)      |   ⬜   |
```

Changes:
- Add `⭐` column as the **first** column (use ⭐ or 💎 per marker rules)
- Rename `Phase` header to `P`
- Change phase values: `**0**` → `P0`, `**3**` → `P3`, etc.
- Shorten `Depends On` references: `Phase 2 (§3.1)` → `P2 (§3.1)`
- Remove any trailing `|` after Status column (stray pipe)
- Align all columns

### 3. Fix the OS Comparison table

**Before:**
```markdown
| Feature              | 🪟 Windows 11       | 🐧 Linux            | 🚀 Impossible OS     |
| -------------------- | -------------------- | -------------------- | --------------------- |
| Custom MBR parsing   | ✅ Full              | ✅ Full              | ⬜ §1.1 P0            |
| **Hybrid MBR**       | ❌ Cannot create     | ⚠️ CLI only          | ⬜ **§7.2 P3 — GUI**  |
```

**After:**
```markdown
| ⭐ | Feature              | 🪟 Windows 11       | 🐧 Linux            | 🚀 Impossible OS     |
| -- | -------------------- | -------------------- | -------------------- | --------------------- |
| 💎 | Custom MBR parsing   | ✅ Full              | ✅ Full              | ⬜ §1.1 P0            |
| ⭐ | **Hybrid MBR**       | ❌ Cannot create     | ⚠️ CLI only          | ⬜ **§7.2 P3 — GUI**  |
```

Changes:
- Add `⭐` column as the **first** column
- Bold feature names that are competitive advantages (⭐ rows)
- Align all columns

### 4. Verify the Priority Order table

The Priority Order table usually already has an `⭐` column. Verify it exists and is aligned. If missing, add it using the same marker rules.

### 5. Align all other tables in the file

Scan the entire file for any other markdown tables (e.g., dependency tables, reference tables). Align their columns consistently.

### 6. Verify rendering

Open the file in the editor preview to confirm all tables render correctly. Check for:
- No stray `|` characters
- Consistent column widths
- Proper alignment markers (`:----:` for centered, `------` for left)

### 7. Save — do NOT commit

This is a formatting-only change. The caller will commit when ready, possibly batching with other TODO file fixes.
