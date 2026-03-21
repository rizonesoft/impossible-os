---
description: Validate documentation structure, check references, and fix violations
---

# Validate Documentation

Run this workflow after any structural change to `docs/` — file moves, renames, new docs, or TODO conversions. It checks for broken links, stale references, orphaned files, and structural violations.

## When to Run

- After converting a TODO to documentation (`/docs-convert-todo`)
- After moving or renaming docs
- After deleting docs
- Periodically as a health check

## Steps

### 1. Scan for broken internal links

Search all `.md` files under `docs/`, `todo/`, and project root for markdown links pointing to files that don't exist:

```bash
# Find all markdown links [text](path) and verify targets exist
grep -rnoP '\[.*?\]\(((?!https?://|mailto:|#)[^)]+)\)' docs/ todo/ README.md CONTRIBUTING.md --include="*.md" \
  | while IFS=: read -r file line match; do
      target=$(echo "$match" | grep -oP '\(([^)]+)\)' | tr -d '()')
      # Resolve relative to file's directory
      dir=$(dirname "$file")
      resolved="$dir/$target"
      # Strip #anchor
      resolved="${resolved%%#*}"
      if [ ! -f "$resolved" ] && [ ! -d "$resolved" ]; then
        echo "BROKEN: $file:$line → $target"
      fi
    done
```

For each broken link:
- If the target was moved, update the link to the new path
- If the target was deleted, remove the link or replace with a note
- If the target is a TODO stub, verify the stub still exists

### 2. Scan for stale TODO references

Search for references to TODO files that have been converted to documentation:

```bash
grep -rn "TODO-[0-9]" docs/ --include="*.md"
```

For each match:
- Check if the referenced TODO has been converted (has a stub with `✅` and `**Documentation:**` link)
- If converted: replace the TODO reference with a link to the new documentation
- If still active: leave the reference as-is
- Exception: references in `todo/` directory itself are fine (stubs cross-reference each other)

### 3. Validate docs directory structure

Check that docs follow the expected organization:

```
docs/
├── architecture/      ← OS internals (kernel, drivers, filesystem, networking, boot, desktop)
├── guides/            ← How-to guides, tutorials, getting-started
├── infrastructure/    ← Build system, CI/CD, tooling, development environment
└── specs/             ← External reference specs (AHCI, FAT32, UEFI, etc.)
```

Violations to check:
- Files directly in `docs/` root (should be in a subdirectory)
- Misplaced files (e.g., a driver doc in `infrastructure/`, a CI doc in `architecture/`)
- Empty directories (clean up after moves)

For each violation:
- Move the file to the correct directory
- Update all references to the file

### 4. Check cross-doc consistency

For each doc, verify:
- **Title matches filename:** `# AHCI Driver` should be in a file like `ahci.md` or `ahci-driver.md`
- **Internal links use relative paths:** no absolute filesystem paths (except in code blocks)
- **Spec references point to `docs/specs/`:** not old `specs/` root path
- **No duplicate content:** two docs shouldn't cover the same component (merge if found)

### 5. Validate TODO index consistency

Check `todo/TODO-000-INDEX.md`:
- Every TODO file in `todo/` subdirectories should have an entry in the index
- Every index entry should point to an existing file
- Converted TODOs should have `✅ Done → [docs](...)` with a valid link

```bash
# Find TODO files not in the index
for f in todo/*/TODO-*.md; do
  base=$(basename "$f" .md | sed 's/TODO-//')
  grep -q "$base" todo/TODO-000-INDEX.md || echo "MISSING FROM INDEX: $f"
done
```

### 6. Fix all violations

For each violation found:
- Fix the issue (move file, update link, add index entry)
- Log what was fixed

### 7. Commit fixes

If any fixes were made:

```bash
git add -A && git commit -m "docs: fix structural violations and broken references"
```

## Quick One-Liner Check

For a fast check without fixes, run:

```bash
# Count broken links in docs/
grep -rnoP '\[.*?\]\(((?!https?://|mailto:|#)[^)]+)\)' docs/ --include="*.md" | \
  while IFS=: read -r file line match; do
    target=$(echo "$match" | grep -oP '\(([^)]+)\)' | tr -d '()')
    dir=$(dirname "$file")
    resolved="$dir/${target%%#*}"
    [ ! -f "$resolved" ] && [ ! -d "$resolved" ] && echo "BROKEN: $file:$line → $target"
  done
```
