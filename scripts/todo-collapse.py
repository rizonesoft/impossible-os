#!/usr/bin/env python3
"""
todo-collapse.py — Collapse completed TODO sections into <details> tags.

Usage:
  python3 scripts/todo-collapse.py [--dry-run] [file ...]

If no files given, processes all todo/**/*.md files recursively.

A section is "completed" when:
  - It contains at least one checkbox (- [x] or - [ ])
  - ALL checkboxes are checked (- [x])
  - No unchecked (- [ ]) items exist

Completed sections are wrapped in:
  <details>
  <summary>✅ N. Section Title (completed)</summary>

  [original content]

  </details>

Already-collapsed sections (<details> immediately following a ## heading) are
skipped to make the script idempotent.

Use --dry-run to preview which sections would be collapsed without modifying
files. Use --uncollapse to reverse the process and expand all collapsed sections
back to plain markdown.
"""

import sys
import os
import re
import glob

def find_todo_files():
    """Find all TODO .md files in the todo/ directory."""
    root = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "todo")
    files = []
    for dirpath, _, filenames in os.walk(root):
        for f in filenames:
            if f.endswith(".md"):
                files.append(os.path.join(dirpath, f))
    return sorted(files)


def parse_sections(lines):
    """
    Parse a markdown file into sections.
    A section starts at a ## heading and ends before the next ## heading or EOF.
    Content before the first ## heading is the "preamble" and is never collapsed.
    """
    sections = []
    current = {"heading_line": -1, "heading": "", "level": 0, "lines": [], "start": 0}

    for i, line in enumerate(lines):
        # Match ## headings (level 2 only — top-level sections)
        m = re.match(r'^(##)\s+(.+)$', line)
        if m and not line.startswith("###"):
            # Save the previous section
            if current["heading_line"] >= 0 or current["lines"]:
                sections.append(current)
            current = {
                "heading_line": i,
                "heading": m.group(2).strip(),
                "level": len(m.group(1)),
                "lines": [line],
                "start": i,
            }
        else:
            current["lines"].append(line)

    # Don't forget the last section
    if current["heading_line"] >= 0 or current["lines"]:
        sections.append(current)

    return sections


def section_is_completed(lines):
    """
    Check if a section has all checkboxes checked.
    Returns True only if there's at least one checkbox and ALL are [x].
    """
    has_checkbox = False
    for line in lines:
        stripped = line.strip()
        if re.match(r'^-\s+\[x\]', stripped):
            has_checkbox = True
        elif re.match(r'^-\s+\[\s?\]', stripped):
            return False  # found unchecked item
    return has_checkbox


def section_already_collapsed(lines):
    """Check if the section is already wrapped in <details>."""
    # Look for <details> tag within the first few non-empty lines after heading
    for line in lines[1:6]:  # skip heading, check next 5 lines
        stripped = line.strip()
        if stripped == "<details>":
            return True
        if stripped and not stripped.startswith(">") and stripped != "---":
            break
    return False


def collapse_section(lines, heading):
    """Wrap section content in <details><summary>."""
    # The heading line itself
    heading_line = lines[0]

    # Strip trailing whitespace/separators from the section content
    content_lines = lines[1:]

    # Remove trailing --- separator and blank lines
    while content_lines and content_lines[-1].strip() in ("---", ""):
        content_lines.pop()

    # Extract a clean title for the summary (remove trailing ✅ and similar)
    clean_title = heading.rstrip()
    # Remove existing ✅ markers to avoid duplication
    clean_title = re.sub(r'\s*✅\s*$', '', clean_title)
    clean_title = re.sub(r'\s*✅\s*→.*$', '', clean_title)

    result = []
    result.append(heading_line)
    result.append("\n")
    result.append(f"<details>\n")
    result.append(f"<summary>✅ {clean_title} — completed</summary>\n")
    result.append("\n")
    result.extend(content_lines)
    result.append("\n")
    result.append("\n")
    result.append("</details>\n")
    result.append("\n")
    result.append("---\n")

    return result


def uncollapse_section(lines):
    """Remove <details> wrapper, restoring plain markdown."""
    result = []
    in_details = False
    skip_next_blank = False

    for line in lines:
        stripped = line.strip()
        if stripped == "<details>":
            in_details = True
            skip_next_blank = True
            continue
        if stripped.startswith("<summary>") and in_details:
            skip_next_blank = True
            continue
        if stripped == "</details>":
            in_details = False
            skip_next_blank = True
            continue
        if skip_next_blank and stripped == "":
            skip_next_blank = False
            continue
        skip_next_blank = False
        result.append(line)

    return result


def process_file(filepath, dry_run=False, uncollapse=False):
    """Process a single TODO file. Returns (collapsed_count, section_names)."""
    with open(filepath, "r") as f:
        lines = f.readlines()

    sections = parse_sections(lines)
    modified = False
    collapsed_sections = []
    new_lines = []

    for section in sections:
        sec_lines = section["lines"]
        heading = section["heading"]

        if uncollapse:
            if section_already_collapsed(sec_lines):
                sec_lines = uncollapse_section(sec_lines)
                modified = True
                collapsed_sections.append(heading)
            new_lines.extend(sec_lines)
            continue

        # Skip preamble (no heading)
        if section["heading_line"] < 0:
            new_lines.extend(sec_lines)
            continue

        # Skip already collapsed
        if section_already_collapsed(sec_lines):
            new_lines.extend(sec_lines)
            continue

        # Check if all items are completed
        if section_is_completed(sec_lines):
            collapsed = collapse_section(sec_lines, heading)
            new_lines.extend(collapsed)
            modified = True
            collapsed_sections.append(heading)
        else:
            new_lines.extend(sec_lines)

    if modified and not dry_run:
        with open(filepath, "w") as f:
            f.writelines(new_lines)

    return len(collapsed_sections), collapsed_sections


def main():
    args = sys.argv[1:]
    dry_run = "--dry-run" in args
    uncollapse_mode = "--uncollapse" in args
    args = [a for a in args if not a.startswith("--")]

    if args:
        files = args
    else:
        files = find_todo_files()

    if not files:
        print("No TODO files found.")
        return

    action = "uncollapse" if uncollapse_mode else "collapse"
    total_collapsed = 0
    total_files = 0

    for filepath in files:
        count, sections = process_file(filepath, dry_run, uncollapse_mode)
        if count > 0:
            total_files += 1
            total_collapsed += count
            rel = os.path.relpath(filepath)
            prefix = "[DRY RUN] " if dry_run else ""
            print(f"{prefix}{rel}: {action}d {count} section(s)")
            for name in sections:
                print(f"  → {name}")

    if total_collapsed == 0:
        print(f"No sections to {action}.")
    else:
        prefix = "Would " if dry_run else ""
        print(f"\n{prefix}{action.title()}d {total_collapsed} section(s) across {total_files} file(s).")


if __name__ == "__main__":
    main()
