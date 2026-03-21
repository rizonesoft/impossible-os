---
description: Check the codebase for completed TODO items, mark them done, and fix consistency issues
---

# TODO Done Check

Cross-reference TODO items against the actual codebase to identify completed work that
hasn't been marked `[x]`. For master TODO files, cascade into child sub-files. Also
fix consistency issues and inaccuracies between TODO descriptions and actual code.

## When to Run

- After a batch of implementation work across multiple TODO files
- When unsure which TODO items have already been implemented
- Before planning the next implementation phase
- When a TODO file's completion status looks stale or inconsistent

## Input

A **TODO file path** — either a single sub-file or a **master** TODO file.

If a **master** file is given, the workflow cascades into all child sub-files
in the corresponding sub-directory.

## Steps

### 1. Read the TODO file

Read the entire TODO file. Extract:
- All sections with their `## N.` / `### N.M` numbers and titles
- Every checkbox item (`- [ ]`, `- [/]`, `- [x]`)
- The expected source files, header files, functions, structs, and macros
  mentioned in prompts and checkbox descriptions

If the input is a **master file**, also:
- List the sub-directory and read the first 50 lines of every child sub-file
- Build a list of all child files and their completion ratios

### 2. Search the codebase for each pending item

For every `- [ ]` (unchecked) or `- [/]` (in-progress) item:

1. **Parse what the item expects** — identify concrete deliverables:
   - Source/header files to create or modify (e.g., `src/kernel/io/iomgr.c`)
   - Functions to implement (e.g., `IoAllocateIrp()`)
   - Structs/enums to define (e.g., `typedef struct _IRP`)
   - Macros/constants to add (e.g., `#define IRP_MJ_CREATE`)
   - Registry keys, config entries, or data tables
   - Serial/debug log messages (e.g., `[AHCI] NCQ depth`)

2. **Search using all available tools:**
   - `grep_search` — exact function names, struct names, macro names
   - `find_by_name` — expected file paths
   - `mcp_srclight_search_symbols` — symbol lookup
   - `mcp_srclight_get_symbol` — full implementation check
   - `view_file` — verify non-stub implementation

3. **Classify each item:**

   | Classification | Criteria | Action |
   |----------------|----------|--------|
   | ✅ **Done**     | All expected code exists, is non-stub, and builds | Mark `[x]` |
   | ⚠️ **Partial** | Some code exists but incomplete or has TODOs/stubs | Mark `[/]`, add note |
   | ⬜ **Pending**  | No matching code found | Leave as `[ ]` |
   | 🔄 **Mismatch** | Code exists but differs from TODO description | Fix TODO description |

### 3. Verify completeness of "done" items

For items you're about to mark `[x]`, verify thoroughly:

- **File exists** at the documented path
- **Function signatures** match (parameter types, return types)
- **No stub implementations** — function body is not just `// TODO` or `return 0;`
- **No compiler warnings** — the code should build cleanly (check `build/build.log`)
- **Cross-references** — if the item says "integrates with X", verify the integration exists

> [!CAUTION]
> **Do NOT mark items `[x]` if the code is a placeholder or stub.** A function
> that exists but returns a hardcoded value or has `// TODO` comments is `[/]`
> (in progress), not done.

### 4. Update checkboxes and add notes

For each item whose status changed:

1. **Mark `[x]`** for fully implemented items:
   ```markdown
   - [x] Implement `IoAllocateIrp()` in `src/kernel/io/iomgr.c`
   ```

2. **Mark `[/]`** for partially implemented items and add a note:
   ```markdown
   - [/] Implement NCQ error recovery
     > **Status:** Error detection implemented in `ahci_port_isr()` but recovery
     > path (FIS clear + re-issue) is not yet wired up. See `ahci.c:487`.
   ```

3. **Fix inaccuracies** in item descriptions:
   ```markdown
   - [ ] Add `virtio_blk_flush()` to `src/kernel/drivers/virtio_blk.c`
     > **Note:** Function exists as `virtio_blk_write_flush()` — name differs
     > from TODO. Updated description to match actual code.
   ```

4. **Update section status indicators** in headings:
   - If all items are `[x]` → add `✅` to the heading
   - If mixed → add `⚠️` or `🔄`
   - If all pending → leave as-is or add `⏳`

### 5. Check consistency and inaccuracies

Scan the entire TODO file for these common issues:

| Issue Type | What to Check | Fix |
|------------|---------------|-----|
| **Wrong file paths** | Source paths in prompts vs. actual repo layout | Update TODO paths |
| **Renamed functions** | Function name in TODO vs. actual symbol name | Update TODO name |
| **Missing includes** | Header files referenced but don't exist yet | Note as pending |
| **Stale status emojis** | Section says ⬜ but all items are `[x]` | Update emoji |
| **Phase table mismatch** | Phase table shows ⬜ but section is complete | Update to ✅ |
| **Priority table mismatch** | Priority says "pending" but work is done | Update to ✅ Done |
| **OS Comparison stale** | Comparison says ⬜ but feature is implemented | Update to ✅ |
| **Cross-ref broken** | XREF points to moved/renamed section | Fix reference |
| **Spec path wrong** | Spec file path doesn't match `docs/specs/` | Fix path |
| **Duplicate items** | Same item appears in multiple sections | Remove duplicate |

### 6. Update master file tables (if master)

If the input was a master TODO file:

1. **Phase-by-Phase table** — update `Status` column:
   - `✅` if all checkboxes in that row's section(s) are `[x]`
   - `⚠️` if mixed `[x]` and `[ ]`
   - `⬜` if all unchecked

2. **Priority Order table** — mark completed entries `✅ Done`

3. **Sub-file priority table** — update summary column to reflect new completion

4. **OS Comparison table** — update `🚀 Impossible OS` column:
   - `✅` for fully implemented features
   - `⚠️` for partially implemented
   - `⬜` for pending

5. **Dependency graph** — update status indicators in node labels

6. **Notes/Tips** — update any notes that reference item status

### 7. Add completion notes to child sub-files (if master)

For each child sub-file where items were marked done:

1. Open the sub-file
2. Add or update a status note at the top (below the goal blockquote):
   ```markdown
   > [!NOTE]
   > **Completion status:** 14/22 items complete (64%). Last checked: 2026-03-21.
   > Sections §1–§4 fully implemented. §5–§7 pending.
   ```
3. Update the sub-file's internal Phase table and Priority table if present
4. Ensure the sub-file's OS Comparison table matches the master's

### 8. Final sweep

Re-read the updated file(s) and verify:
- [ ] No `[x]` items that reference non-existent code
- [ ] No `[ ]` items where the code clearly exists
- [ ] All status emojis match actual completion
- [ ] All tables are internally consistent
- [ ] Phase table status matches section completion
- [ ] Priority table status matches section completion
- [ ] OS Comparison reflects actual implementation state
- [ ] All file paths in prompts are accurate
- [ ] All function/struct names match actual code

### 9. Commit

```bash
git add -A && git commit -m "todo: mark completed items in <file-name>

- Marked N items as done ([x])
- Marked N items as in-progress ([/])
- Fixed N inaccuracies (paths, names, status)
- Updated phase/priority/comparison tables
- <brief summary of key completions>"
```

## Tips

- **Use Srclight MCP** (`search_symbols`, `get_symbol`, `hybrid_search`) for fast
  symbol lookups — much faster than manual grep for function signatures.
- **Check `build/build.log`** to verify compilation status of files mentioned in TODOs.
- **Don't assume** — always verify with actual code. A function name in a TODO prompt
  may have been implemented under a different name.
- **Be conservative** — when in doubt, mark as `[/]` (in-progress) rather than `[x]`.
  False positives (marking incomplete work as done) are worse than false negatives.
- **Batch sub-file updates** — when processing a master, handle all child files before
  updating the master tables, so the master reflects the true aggregate state.
