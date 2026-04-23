#!/usr/bin/env python3
# ============================================================================
# scripts/todo-graph/migrate-add-frontmatter.py -- TODO frontmatter back-fill.
#
# Owner: TODO-06 §5 (Migration) in todo/00-infrastructure/.
# One-time sweep that adds the §1 frontmatter block to every TODO file
# that currently lacks one. Mechanical + idempotent + dry-run-first.
#
# For each TODO-*.md file under todo/ (excluding INDEX + TEMPLATE + any
# file already carrying a frontmatter block):
#
#   1. Derive `id` from the filename stem's `TODO-NN-<slug>` suffix
#      (e.g. TODO-02-ai-development-system.md -> id: ai-development-system).
#   2. Derive `domain` from the parent directory (e.g. 00-infrastructure).
#   3. Derive `title` from the first `# ` H1 line, with the leading `# `
#      stripped. Fallback to the filename stem when no H1 exists.
#   4. Default `status: active` (humans promote to done / draft / blocked
#      in a follow-up pass; --status lets a bulk caller override).
#
# depends_on / satisfies / owners / file_patterns / superseded_by stay
# unset -- the spec explicitly keeps dependency inference out of scope
# (humans do that pass-by-hand in a review cycle).
#
# CLI:
#   migrate-add-frontmatter.py [--root PATH] [--dry-run] [--status STATUS]
#                              [--domain DOMAIN]
#
# --root PATH     : directory to walk (default: todo/). Use a single
#                   domain directory (e.g. todo/00-infrastructure/) to
#                   migrate one batch at a time.
# --dry-run       : print the proposed diff-sized preview; do not write.
# --status STATUS : override the default `status: active` value.
# --domain DOMAIN : restrict the walk to a single domain slug, even if
#                   --root points at todo/. Useful for one-command
#                   per-domain batches: `--root todo --domain 02-kernel-core`.
#
# Idempotence: files whose first non-BOM line is `---` are skipped.
# Re-running the script produces zero changes on a fully-migrated tree.
#
# Opt-out: TODO-00-INDEX.md and per-domain INDEX.md files are never
# TODO leaves and are excluded by the walk (the frontmatter spec only
# covers TODO-NN-<slug>.md files).
# ============================================================================

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Optional


# Filename stem like `TODO-06-todo-metadata-layer` -- the slug after
# the TODO-NN- prefix is what becomes the frontmatter id. Master-table
# TODOs use a single uppercase letter (TODO-A-, TODO-B-, etc) per the
# §3 validator's convention for registry/SSDT-style reference docs; we
# accept either number or single uppercase letter here and lowercase
# the whole slug for the id.
FILENAME_SLUG_RE = re.compile(r"^TODO-(?:\d{1,2}|[A-Z])-(?P<slug>.+)$")

# H1 extractor. First line of the form `# ...` (exactly one #) wins.
H1_RE = re.compile(r"^#\s+(?P<title>.+?)\s*$")

# Kebab-case id validator -- mirrors the schema regex.
ID_RE = re.compile(r"^[a-z][a-z0-9-]{0,58}[a-z0-9]$")


def derive_id(filename_stem: str) -> Optional[str]:
    m = FILENAME_SLUG_RE.match(filename_stem)
    if not m:
        return None
    slug = m.group("slug").lower()
    return slug if ID_RE.match(slug) else None


def derive_title(body: str, filename_stem: str) -> str:
    for line in body.splitlines():
        m = H1_RE.match(line)
        if m:
            return m.group("title").strip()
    # Fallback: use the filename stem verbatim (caller can hand-edit later).
    return filename_stem


def has_frontmatter(text: str) -> bool:
    """Return True when the file already carries a YAML frontmatter
    block. Tolerates an optional UTF-8 BOM and leading blank lines
    before the opening `---` fence (Codex pass 12 M1 -- a file with a
    BOM + blank-line drift would otherwise get a SECOND frontmatter
    block prepended, silently orphaning the original metadata)."""
    if not text:
        return False
    if text.startswith("﻿"):
        text = text[1:]
    for line in text.splitlines():
        stripped = line.rstrip("\r").strip()
        if not stripped:
            continue
        return stripped == "---"
    return False


def _yaml_double_quote(s: str) -> str:
    """Emit `s` as a valid YAML double-quoted scalar. Backslashes and
    double-quotes are the two escape introducers that matter; also
    strip/replace control characters so the emitted line stays on one
    line. Simpler than pulling in PyYAML's emitter for a four-line
    block, and avoids the Codex pass 12 H1 case where titles like
    `C:\\Temp` produced invalid YAML (`\\T` is not a legal escape)."""
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch in ("\n", "\r", "\t"):
            out.append(" ")
        elif ord(ch) < 0x20:
            # Other control chars -> skip (unusual in titles but defensive).
            continue
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def build_frontmatter_block(id_: str, domain: str, status: str, title: str) -> str:
    """Emit the canonical frontmatter block. Key order mirrors the spec
    doc's authoritative example: schema_version, id, domain, status,
    title. Leaves the optional fields unset."""
    return (
        "---\n"
        "schema_version: 1\n"
        f"id: {id_}\n"
        f"domain: {domain}\n"
        f"status: {status}\n"
        f"title: {_yaml_double_quote(title)}\n"
        "---\n\n"
    )


def walk_todo_files(root: Path, domain_filter: Optional[str]) -> list:
    out = []
    for path in sorted(root.rglob("TODO-*.md")):
        name = path.name
        # Skip INDEX files -- TODO-00-INDEX.md is the roadmap root, not
        # a TODO leaf with a checklist.
        if name == "TODO-00-INDEX.md" or name.endswith("-INDEX.md"):
            continue
        if domain_filter:
            parts = path.relative_to(root).parts if path.is_relative_to(root) else path.parts
            # Expect TODO files at todo/<domain>/TODO-*.md; the domain
            # is the second-to-last path segment relative to root.
            if len(parts) >= 2 and parts[-2] != domain_filter:
                continue
            if len(parts) == 1:
                # Flat layout (e.g. --root pointed inside a domain);
                # compare the root's own name to domain_filter.
                if root.name != domain_filter:
                    continue
        out.append(path)
    return out


def process_file(path: Path, status: str, dry_run: bool, repo_root: Path) -> str:
    """Return a short status token describing what happened:
      - 'already-fm' : skipped; already has frontmatter
      - 'would-add'  : dry-run; frontmatter would be prepended
      - 'added'      : frontmatter prepended and file written
      - 'skip-no-id' : filename does not match TODO-NN-<slug> convention
      - 'skip-no-dom': could not derive domain from parent directory
    """
    text = path.read_text(encoding="utf-8")
    if has_frontmatter(text):
        return "already-fm"

    stem = path.stem
    id_ = derive_id(stem)
    if not id_:
        sys.stderr.write(f"[migrate] WARN: {path} has unparseable filename stem '{stem}'; skipping\n")
        return "skip-no-id"

    # Domain = parent directory's name (e.g. 00-infrastructure).
    try:
        rel = path.relative_to(repo_root)
        parts = rel.parts
    except ValueError:
        parts = path.parts
    # Expect at least two segments: <domain>/<file>.md
    if len(parts) < 2:
        sys.stderr.write(f"[migrate] WARN: {path} has no domain segment; skipping\n")
        return "skip-no-dom"
    domain = parts[-2]

    title = derive_title(text, stem)
    block = build_frontmatter_block(id_, domain, status, title)

    if dry_run:
        sys.stdout.write(f"--- would add to {path} ---\n")
        sys.stdout.write(block)
        return "would-add"

    new_text = block + text
    # Atomic write: unique temp file in the same directory + os.replace
    # guarantees either the old content or the new content on disk --
    # never a half-written file. Codex pass 12 M2 flagged the fixed
    # <name>.md.tmp path as a clobber hazard (a pre-existing sibling
    # .tmp file from another tool would be silently overwritten and
    # then deleted by the OSError cleanup); pass 13 M1 fixed it by
    # using tempfile.NamedTemporaryFile for an exclusively-created
    # unique name in path.parent.
    tmp_fd, tmp_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=str(path.parent),
    )
    tmp_path = Path(tmp_name)
    # Codex pass 14 M1: catch EVERY exception, not just OSError, so a
    # UnicodeEncodeError on an invalid Unicode scalar (lone surrogate,
    # etc) inside the TODO body still cleans up the hidden temp file.
    try:
        with os.fdopen(tmp_fd, "w", encoding="utf-8") as f:
            f.write(new_text)
        os.replace(tmp_path, path)
    except BaseException:
        # Close the fd if the fdopen context manager didn't take
        # ownership (fdopen itself raised). Best-effort -- ignore
        # secondary failures from a stale fd.
        try:
            os.close(tmp_fd)
        except OSError:
            pass
        if tmp_path.exists():
            try:
                tmp_path.unlink()
            except OSError:
                pass
        raise
    return "added"


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        prog="migrate-add-frontmatter.py",
        description="Back-fill frontmatter on existing TODO files (TODO-06 §5).",
    )
    p.add_argument("--root", default="todo",
                   help="directory to walk (default: todo/)")
    p.add_argument("--dry-run", action="store_true",
                   help="print proposed frontmatter without writing")
    # Match the schema enum in docs/infrastructure/todo-metadata.schema.json.
    # Codex pass 12 H2: without `choices=`, a typo like `--status blockedd`
    # would silently poison every migrated file and immediately brick the
    # §5-hardened build.py.
    p.add_argument("--status", default="active",
                   choices=("draft", "active", "blocked", "done", "superseded"),
                   help="default status for new frontmatter (default: active)")
    p.add_argument("--domain", default=None,
                   help="restrict the walk to a single domain (e.g. 00-infrastructure)")
    p.add_argument("--repo-root", default=None,
                   help="repo root override (default: auto-detect relative to --root)")
    args = p.parse_args(argv)

    root = Path(args.root).resolve()
    if not root.exists():
        sys.stderr.write(f"[migrate] FATAL: --root {root} does not exist\n")
        return 2

    # Determine the repo root so domain extraction works. If --root is
    # todo/ at the repo top, repo_root = root.parent. Otherwise use the
    # explicit override or walk upward.
    if args.repo_root:
        repo_root = Path(args.repo_root).resolve()
    else:
        # If root contains `todo/` itself, the parent is the repo.
        # Else, if root IS `todo/`, the parent is the repo.
        if root.name == "todo":
            repo_root = root.parent
        elif (root / "todo").is_dir():
            repo_root = root
            root = root / "todo"
        else:
            # --root points inside todo/; walk up to find the todo parent.
            for cand in (root, *root.parents):
                if cand.name == "todo":
                    repo_root = cand.parent
                    break
            else:
                repo_root = root

    files = walk_todo_files(root, args.domain)
    if not files:
        sys.stdout.write(f"[migrate] no TODO files matched under {root}"
                         + (f" (domain={args.domain})" if args.domain else "")
                         + "\n")
        return 0

    counts = {"already-fm": 0, "would-add": 0, "added": 0,
              "skip-no-id": 0, "skip-no-dom": 0}
    for path in files:
        try:
            rel_for_domain = path.relative_to(repo_root / "todo")
            parts = (repo_root / "todo").parts + rel_for_domain.parts
            local_repo_root = repo_root / "todo"
        except ValueError:
            local_repo_root = repo_root
        status = process_file(path, args.status, args.dry_run, local_repo_root)
        counts[status] = counts.get(status, 0) + 1

    summary = ", ".join(f"{k}={v}" for k, v in counts.items() if v)
    sys.stdout.write(f"[migrate] {summary} ({'dry-run' if args.dry_run else 'write'})\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
