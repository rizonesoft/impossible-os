---
name: validate-doc
description: Fact-check a documentation file against the codebase — verify function signatures, file paths, counts, commands, and completeness. Ensure it has charts, tables, OS Comparison, and meets the quality standard. Use when reviewing or auditing docs.
---

# Validate Documentation

## When to Use

- After creating or updating a doc file in `docs/`
- When the user asks to "validate docs", "check the docs", or "audit documentation"
- Periodically to catch drift between docs and code
- After a `/complete-todo` conversion to verify the output

## Workflow

### 1. Read the doc file

Read the entire document. Note every factual claim:
- Function signatures and parameter types
- File paths and directory structures
- Numeric counts (suites, assertions, types, sizes)
- Command examples and their expected output
- Configuration values and defaults
- Architecture descriptions

### 2. Fact-check against the codebase

For each claim, verify against actual source:

| Claim Type | How to Verify |
|---|---|
| Function signature | `grep` the header file for the exact declaration |
| File exists | `ls` or `glob` for the path |
| Directory structure | `find` or `ls -R` the directory |
| Numeric count | `grep -c` or run the relevant script |
| Boot.conf setting | Read `resources/boot/boot.conf` |
| Makefile target | `grep` the Makefile |
| Build command | Dry-run or verify script exists |
| Config struct | Read the header definition |

**Mark each claim:**
- CORRECT — matches codebase
- STALE — was true but code has changed
- WRONG — never matched or has a typo
- UNVERIFIABLE — can't check from code alone (runtime behavior)

### 3. Check document completeness

Every doc should have these sections (quality standard from `docs/infrastructure/development-tooling.md`):

| Section | Required? | What to Check |
|---|---|---|
| Title + summary blockquote | Yes | Clear one-line description |
| Overview | Yes | Explains what and why |
| Mermaid diagram | Yes | At least one architecture or flow diagram |
| Core content sections | Yes | Tables, code examples, explanations |
| Key Files table | Yes | File path + purpose for every relevant file |
| Gotchas | Recommended | Hard-won lessons, common mistakes |
| OS Comparison table | Yes | Win11 / Linux / Impossible OS columns |
| References | Recommended | Links to source, related docs, specs |
| Listed in folder index.md | Yes | Doc appears in `docs/<subfolder>/index.md` Documents table |

### 4. Check for missing content

Look for gaps:
- **Missing diagrams** — any complex flow or architecture without a Mermaid chart?
- **Missing tables** — any list of items that would be clearer as a table?
- **Missing OS Comparison rows** — any feature mentioned in the doc that's not in the comparison?
- **Missing gotchas** — any `> [!CAUTION]` or `> [!WARNING]` items from CLAUDE.md or session memories that should be documented?
- **Stale screenshots or counts** — numbers that were correct when written but have changed?

### 5. Check for planning artifacts

Docs should NOT contain:
- `[ ]` or `[x]` checkboxes (TODO artifacts)
- "Commit:" lines
- "Test checkpoint:" lines
- `→ XREF:` references (convert to proper doc links)
- "Planned", "will be", "deferred" language (docs describe what IS)
- Section numbers from the TODO (`§1`, `§2`)

### 6. Report findings

Format the report as:

```
Document: docs/<path>

Fact Check:
  CORRECT: N claims verified
  STALE: M claims need updating
    - <claim> — was X, now Y
  WRONG: K claims are incorrect
    - <claim> — says X, actually Y

Completeness:
  [x] Title + summary
  [x] Overview with Mermaid diagram
  [x] Core sections with tables
  [ ] Missing: Key Files table
  [x] OS Comparison table
  [ ] Missing: Gotchas section

Planning Artifacts:
  - Line 42: contains "§3" reference (remove)
  - Line 87: contains "deferred" language (rewrite)

Suggestions:
  - Add Mermaid diagram showing <X> data flow
  - Add table for <Y> configuration options
  - Update assertion count from 59 to 96
```

### 7. Fix issues (if requested)

If the user asks to fix the issues, make the edits:
- Update stale facts with current values from the codebase
- Add missing sections (diagrams, tables, OS Comparison rows)
- Remove planning artifacts
- Rewrite speculative language as factual statements

## Guardrails

- Do NOT change docs without reporting findings first (unless the user asked to "fix")
- Do NOT add speculative content — only document what exists in the codebase
- Do NOT remove content that's correct — only fix what's wrong
- If a claim can't be verified, mark it UNVERIFIABLE, don't mark it WRONG
- Always verify against the CURRENT codebase, not memory of what it used to be
