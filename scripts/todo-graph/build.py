#!/usr/bin/env python3
# ============================================================================
# build.py -- TODO graph cache generator
#
# Walks every `todo/**/*.md` (excluding INDEX.md / TODO-00-INDEX.md), parses
# YAML frontmatter + Implementation Order table rows + Inputs XREFs +
# Accepted/Deferred stamp XREFs, then emits a JSON array of node objects to
# `build/todo-cache.json`.
#
# Spec: the TODO-graph generator section of the TODO-metadata-layer plan
# under todo/00-infrastructure/. Frontmatter schema spec lives in the
# adjacent schema-and-spec section, with a JSON Schema sidecar at
# docs/infrastructure/todo-metadata.schema.json once that section ships.
#
# Usage:
#   python3 scripts/todo-graph/build.py [--output build/todo-cache.json]
#                                       [--root todo/]
#                                       [--quiet]
#
# Exit codes:
#   0  clean parse of every file
#   1  any file's frontmatter is malformed (file:line + category printed to
#      stderr); cache file is NOT written so a partial cache cannot mask a
#      regression
#
# Performance target: under 2s wall-clock for ~86 TODO files. Strategy:
#   - Single batched `git log` call gets created_at + last_active_at
#     timestamps for every file in one process invocation.
#   - YAML parsing uses PyYAML (declared in scripts/setup-deps.sh).
#   - All other parsing is pure-stdlib regex + string ops.
#
# Today the repo has 223 tracked TODO files; even at that scale the
# expected wall-clock is well under 2s on a dev machine.
# ============================================================================

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

try:
    import yaml
except ImportError:
    sys.stderr.write(
        "[build.py] FATAL: python3-yaml not installed. Run: bash scripts/setup-deps.sh\n"
    )
    sys.exit(1)


# PyYAML's default safe loader uses last-key-wins on duplicate mapping keys.
# The frontmatter spec (docs/infrastructure/todo-metadata.md Parsing Rules)
# promises duplicate keys are a hard malformed-yaml failure. Subclass
# SafeLoader and override mapping construction to raise on the first duplicate.
class _DupRejectingLoader(yaml.SafeLoader):
    pass


def _construct_mapping_no_dupes(loader, node, deep=False):
    mapping = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in mapping:
            raise yaml.constructor.ConstructorError(
                None,
                None,
                f"duplicate key {key!r} in mapping",
                key_node.start_mark,
            )
        mapping[key] = loader.construct_object(value_node, deep=deep)
    return mapping


_DupRejectingLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG,
    _construct_mapping_no_dupes,
)


# --- Constants -----------------------------------------------------------

VALID_STATUSES = {"draft", "active", "blocked", "done", "superseded"}
ID_REGEX = re.compile(r"^[a-z][a-z0-9-]{0,58}[a-z0-9]$")
REQUIRED_FIELDS = ("schema_version", "id", "domain", "status", "title")
OPTIONAL_FIELDS = (
    "$schema",
    "owners",
    "depends_on",
    "satisfies",
    "superseded_by",
    "file_patterns",
)
SUPPORTED_SCHEMA_VERSION = 1

# `> **Accepted:**` / `> **Deferred:**` line header. Captures kind +
# optional severity. The XREF clauses themselves are extracted in a
# second pass (XREF_CLAUSE_RE below) because some live stamp lines
# carry MULTIPLE `-> XREF:` clauses on the same line (TODO-12 SSDT
# stamps have up to 6); a single regex match only catches the first.
STAMP_HEADER_RE = re.compile(
    r"^> \*\*(?P<kind>Accepted|Deferred):\*\*\s+"
    r"(?:\[(?P<severity>Critical|H|M|L)\]\s+)?",
    re.MULTILINE,
)

# Single XREF clause within a stamp line. Captures one target_path /
# target_section / optional item_name. Iterated to extract every clause
# on the same line.
XREF_CLAUSE_RE = re.compile(
    r"->\s+XREF:\s+(?P<target_path>\S+)\s+(?P<target_section>§\S+)"
    r"(?:\s*\((?:[^()]*?\b(?:item|new\s+item):\s*\"(?P<item_name>[^\"]+)\")?[^()]*\))?"
)

# Inputs XREF line. Two surface forms in the wild:
#   - plain-text reference (compact TNN section-prefix shorthand)
#   - `- -> XREF: [link text](markdown-path) -- description`
# Extract the target slug + section number where present.
INPUTS_XREF_RE = re.compile(
    r"^-\s+->\s+XREF:\s+(?:\[[^\]]+\]\(([^)]+)\)|`?([^`\s]+))(?:[`\s]+(§\S+))?",
    re.MULTILINE,
)

# Implementation Order header: detect optional Section column.
# Looking for table row patterns under `## Implementation Order`.
# Implementation Order header aliases (live repo formats vary). The Section
# column names the body section number (`§N`); the Deliverable column names
# the work item; Depends names predecessor refs; Status holds the `[ ]`
# checkbox. When the only "what is this" column is `Section` (e.g. live
# 4-column `Step | Section | Tag | Dependency` tables), `Section` doubles
# as the deliverable label and `n` is computed from the Step/# column.
IO_HEADER_KEYS_SECTION = ("section",)
IO_HEADER_KEYS_DELIVERABLE = ("deliverable",)
IO_HEADER_KEYS_DEPS = ("depends on", "depends-on", "dependency", "dep", "deps")
IO_HEADER_KEYS_STATUS = ("status", "mark")
IO_HEADER_KEYS_ORDER = ("order", "step", "#")


# --- File discovery ------------------------------------------------------


def walk_todo_files(root: Path) -> list:
    """Return list of TODO-*.md paths under root, excluding INDEX.md and
    TODO-00-INDEX.md. Sorted for determinism (byte-identical output on
    re-runs, per the TODO-metadata-layer idempotency requirement)."""
    out = []
    for p in root.rglob("TODO-*.md"):
        if p.name == "TODO-00-INDEX.md":
            continue
        out.append(p)
    return sorted(out)


# --- Git timestamps (batched) -------------------------------------------


def collect_git_timestamps(repo_root: Path, files: list) -> dict:
    """Return {relative_path_str: (created_at_iso, last_active_at_iso)} for
    every file. One subprocess call walks the entire log so we don't pay
    per-file fork overhead.

    Strategy: `git log --name-only --format=COMMIT %H %ct --reverse` then
    walk the output, recording the FIRST timestamp we see for each path
    (created_at) and continuously updating to the LATEST (last_active_at).
    `--reverse` makes the first-seen=oldest invariant hold without
    sorting per-file.

    Files with no git history (untracked or new) get None timestamps.
    """
    rel_paths = set()
    for f in files:
        # Files may live outside the repo (e.g. test fixtures under /tmp).
        # Skip git timestamp collection for those; they get None timestamps.
        try:
            rel = f.relative_to(repo_root)
            rel_paths.add(str(rel))
        except ValueError:
            continue

    created = {}
    last_active = {}
    # Path-limit the log walk to `todo/` so runtime scales with TODO
    # history alone, not total-repo history. Without `-- todo/` the
    # `--reverse` walk visits every commit in the repo (kernel churn,
    # docs churn, scripts churn) before producing the first TODO
    # timestamp, which makes the 2s budget brittle as the repo grows.
    # Codex quality review caught this: a full-repo scan was the
    # observed bottleneck once commit count crosses ~10x.
    try:
        result = subprocess.run(
            [
                "git",
                "log",
                "--name-only",
                "--format=COMMIT %H %ct",
                "--reverse",
                "--",
                "todo",
            ],
            cwd=str(repo_root),
            capture_output=True,
            text=True,
            check=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        # Git unavailable or repo broken: emit empty timestamps (caller
        # treats None as unknown, generator still succeeds).
        return {p: (None, None) for p in rel_paths}

    cur_ts = None
    for line in result.stdout.splitlines():
        if line.startswith("COMMIT "):
            parts = line.split(" ", 2)
            if len(parts) >= 3:
                try:
                    cur_ts = int(parts[2])
                except ValueError:
                    cur_ts = None
            else:
                cur_ts = None
        elif cur_ts is not None and line in rel_paths:
            if line not in created:
                created[line] = cur_ts
            last_active[line] = cur_ts

    out = {}
    for p in rel_paths:
        c = created.get(p)
        l = last_active.get(p)
        out[p] = (
            time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(c)) if c else None,
            time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(l)) if l else None,
        )
    return out


# --- Frontmatter ---------------------------------------------------------


def split_frontmatter(content: str):
    """Return (frontmatter_dict, body, error_msg). All three are None-able:
       - (dict, body, None) on parse success
       - (None, full_content, None) when no frontmatter present (back-compat
         no-frontmatter mode)
       - (None, content, error_msg) on malformed YAML

    Tolerates:
      - UTF-8 BOM prefix (Windows editors save with BOM by default)
      - CRLF line endings (`---\\r\\n`) -- normalized to LF for the
        fence search so a Windows-authored TODO with valid frontmatter
        is parsed instead of misclassified as no-frontmatter.
    """
    # Strip UTF-8 BOM if present (Windows editors).
    if content.startswith("\ufeff"):
        content = content[1:]
    # Normalize CRLF to LF for fence detection. Body line endings are
    # preserved by working off the original content for the body slice
    # only when normalization changed nothing; otherwise we work entirely
    # in normalized form (which is fine -- downstream parsers accept LF).
    if "\r\n" in content:
        content = content.replace("\r\n", "\n")
    if not content.startswith("---\n"):
        return None, content, None
    # Find closing fence on its own line.
    end = content.find("\n---\n", 4)
    if end < 0:
        return None, content, "frontmatter: opening --- has no closing --- fence"
    yaml_block = content[4:end]
    body = content[end + 5 :]
    # Detect tabs (PyYAML treats them as syntax errors but the message is
    # cryptic; produce a clearer one).
    for ln_idx, ln in enumerate(yaml_block.splitlines(), start=2):
        if "\t" in ln:
            return None, body, f"frontmatter: tab indentation at line {ln_idx} (use spaces)"
    try:
        fm = yaml.load(yaml_block, Loader=_DupRejectingLoader)
    except yaml.YAMLError as exc:
        # PyYAML's exception carries a problem_mark; re-base its line to
        # the file's coordinates (frontmatter starts at line 2).
        if hasattr(exc, "problem_mark") and exc.problem_mark is not None:
            ln = exc.problem_mark.line + 2
            return None, body, f"frontmatter: malformed YAML at line {ln}: {exc.problem}"
        return None, body, f"frontmatter: malformed YAML: {exc}"
    if not isinstance(fm, dict):
        return None, body, "frontmatter: top-level must be a mapping (key: value pairs)"
    # YAML aliases can produce self-referential containers (e.g. `owners: &a [*a]`).
    # PyYAML's safe_load happily constructs them; downstream json.dumps then
    # crashes with an uncaught circular reference. Walk the parsed structure
    # and reject any cycle as malformed-yaml so the generator stays fail-closed.
    if _has_yaml_cycle(fm):
        return None, body, "frontmatter: circular reference (recursive YAML alias) in frontmatter"
    return fm, body, None


def _has_yaml_cycle(obj, _seen=None):
    """Return True if obj contains a self-referential dict or list, walking
    only containers (dict / list). Strings, ints, etc. are leaves."""
    if _seen is None:
        _seen = set()
    if isinstance(obj, (dict, list)):
        if id(obj) in _seen:
            return True
        _seen.add(id(obj))
        children = obj.values() if isinstance(obj, dict) else obj
        for child in children:
            if _has_yaml_cycle(child, _seen):
                return True
        _seen.discard(id(obj))
    return False


def validate_frontmatter(fm: dict) -> list:
    """Return list of (category, message) tuples. Empty list = clean.

    Categories from §2 error catalog (mirrors docs/infrastructure/todo-metadata.md):
      malformed-yaml    (handled separately in split_frontmatter; also covers
                         circular YAML alias cycles via _has_yaml_cycle)
      unknown-status    : status not in VALID_STATUSES
      invalid-id        : id fails regex
      missing-required  : a REQUIRED_FIELDS entry absent
      schema-version    : schema_version not int OR > SUPPORTED_SCHEMA_VERSION
      forbidden-field   : hand-authored cache-only field (created_at /
                          last_active_at) present
      invalid-field     : optional field present but wrong type/shape
                          (mirrors the JSON Schema sidecar's per-field
                          constraints so generator + schema agree)
    Unknown optional fields produce a WARNING (returned with category
    'unknown-field'), not an error; caller logs but does not fail."""
    errors = []
    for field in REQUIRED_FIELDS:
        if field not in fm:
            errors.append(("missing-required", f"required field '{field}' absent"))
    if "status" in fm and fm["status"] not in VALID_STATUSES:
        errors.append(
            (
                "unknown-status",
                f"status='{fm['status']}' not one of {sorted(VALID_STATUSES)}",
            )
        )
    if "id" in fm:
        ident = fm["id"]
        if not isinstance(ident, str) or not ID_REGEX.match(ident):
            errors.append(
                (
                    "invalid-id",
                    f"id='{ident}' must match {ID_REGEX.pattern} "
                    "(lowercase letter start, [a-z0-9-], 2-60 chars, no trailing -)",
                )
            )
    if "schema_version" in fm:
        ver = fm["schema_version"]
        # bool is a subclass of int in Python; True/False would otherwise
        # silently pass the int check. Reject explicitly.
        if not isinstance(ver, int) or isinstance(ver, bool):
            errors.append(
                (
                    "schema-version",
                    f"schema_version='{ver}' must be an integer",
                )
            )
        elif ver < 1:
            errors.append(
                (
                    "schema-version",
                    f"schema_version={ver} must be >= 1 (schema starts at 1)",
                )
            )
        elif ver > SUPPORTED_SCHEMA_VERSION:
            errors.append(
                (
                    "schema-version",
                    f"schema_version={ver} > generator-supported {SUPPORTED_SCHEMA_VERSION} "
                    "(forward-incompatible; upgrade scripts/todo-graph/build.py)",
                )
            )
    # FATAL: cache-only auto-derived fields must NOT appear in hand-authored
    # frontmatter (mirrors the JSON Schema sidecar's not/anyOf/required clause
    # at docs/infrastructure/todo-metadata.schema.json). The generator computes
    # both from git log and emits them into build/todo-cache.json only.
    for cache_only in ("created_at", "last_active_at"):
        if cache_only in fm:
            errors.append(
                (
                    "forbidden-field",
                    f"field '{cache_only}' is auto-derived (cache-only); "
                    "MUST NOT appear in hand-authored frontmatter "
                    "(see docs/infrastructure/todo-metadata.md Auto-Derived Fields)",
                )
            )
    # FATAL: optional field type/shape mismatches. Mirrors the JSON Schema
    # sidecar so the generator and the schema enforce identical contracts on
    # what a clean cache node looks like (no schema/generator split brain).
    if "title" in fm:
        if not isinstance(fm["title"], str):
            errors.append(("invalid-field", f"title must be a string, got {type(fm['title']).__name__}"))
        elif not fm["title"]:
            errors.append(("invalid-field", "title must be a non-empty string (schema minLength: 1)"))
    if "domain" in fm:
        if not isinstance(fm["domain"], str):
            errors.append(("invalid-field", f"domain must be a string, got {type(fm['domain']).__name__}"))
        elif not fm["domain"]:
            errors.append(("invalid-field", "domain must be a non-empty string (schema minLength: 1)"))
    if "$schema" in fm and not isinstance(fm["$schema"], str):
        errors.append(("invalid-field", f"$schema must be a string path, got {type(fm['$schema']).__name__}"))
    if "owners" in fm:
        owners = fm["owners"]
        if not isinstance(owners, list) or not all(isinstance(x, str) for x in owners):
            errors.append(("invalid-field", "owners must be a list of strings"))
    for id_list_field in ("depends_on", "satisfies"):
        if id_list_field in fm:
            val = fm[id_list_field]
            if not isinstance(val, list) or not all(isinstance(x, str) for x in val):
                errors.append(("invalid-field", f"{id_list_field} must be a list of id strings"))
            else:
                for ref in val:
                    if not ID_REGEX.match(ref):
                        errors.append(
                            (
                                "invalid-field",
                                f"{id_list_field}[..] entry '{ref}' must match {ID_REGEX.pattern}",
                            )
                        )
                if len(val) != len(set(val)):
                    errors.append(
                        ("invalid-field", f"{id_list_field} must be uniqueItems (schema uniqueItems: true)")
                    )
    if "superseded_by" in fm:
        sb = fm["superseded_by"]
        if not isinstance(sb, str) or not ID_REGEX.match(sb):
            errors.append(
                ("invalid-field", f"superseded_by='{sb}' must be a single id string matching {ID_REGEX.pattern}")
            )
    if "file_patterns" in fm:
        fp = fm["file_patterns"]
        if not isinstance(fp, list) or not all(isinstance(x, str) and x for x in fp):
            errors.append(("invalid-field", "file_patterns must be a list of non-empty glob strings"))
        elif len(fp) != len(set(fp)):
            errors.append(("invalid-field", "file_patterns must be uniqueItems (schema uniqueItems: true)"))
    # Warnings (don't fail the build)
    known = set(REQUIRED_FIELDS) | set(OPTIONAL_FIELDS)
    for field in fm:
        if field not in known and field not in ("created_at", "last_active_at"):
            errors.append(("unknown-field", f"unknown optional field '{field}' (forward-compat warning)"))
    return errors


# --- Body parsing --------------------------------------------------------


def extract_title(content: str) -> Optional[str]:
    """First `# ` heading, stripped. Returns None if absent."""
    for ln in content.splitlines():
        if ln.startswith("# "):
            return ln[2:].strip()
    return None


def extract_section_headings(body: str) -> list:
    """Return list of (n, title) tuples for every `## N. Title` heading.
    `n` is the integer section number; `title` is the prose after the dot."""
    out = []
    for ln in body.splitlines():
        m = re.match(r"^## (\d+)\.\s+(.+?)\s*$", ln)
        if m:
            out.append((int(m.group(1)), m.group(2)))
    return out


def extract_implementation_order(body: str) -> list:
    """Walk under `## Implementation Order` and return list of section dicts:
       {n: int|None, deliverable: str, depends_on: [str], status: str}.

    Tolerates both 5-column tables (icon | order | deliverable | depends |
    status) and 6-column (icon | order | section | deliverable | depends |
    status). The Section column, when present, names the body section
    (`§N`) which can differ from execution Order.
    """
    out = []
    in_io = False
    header_cols = None
    for ln in body.splitlines():
        if ln.startswith("## Implementation Order"):
            in_io = True
            continue
        if in_io and ln.startswith("## "):
            break
        if not in_io:
            continue
        if not ln.startswith("|"):
            continue
        # Skip separator row (cells like `| --- | :---: | ... |`)
        cells = [c.strip() for c in ln.strip("|").split("|")]
        if all(re.match(r"^[:\-]+$", c) for c in cells if c):
            continue
        if header_cols is None:
            header_cols = [c.lower().strip() for c in cells]
            continue
        # Data row
        row = dict(zip(header_cols, cells))
        # Find columns by fuzzy match. Order matters: more-specific keys
        # win over generic ones via the if/elif chain.
        section_cell = None
        deliverable_cell = None
        depends_cell = None
        status_cell = None
        order_cell = None
        for k, v in row.items():
            if any(key == k or key in k for key in IO_HEADER_KEYS_DELIVERABLE):
                deliverable_cell = v
            elif any(key == k for key in IO_HEADER_KEYS_SECTION):
                section_cell = v
            elif any(key == k or key in k for key in IO_HEADER_KEYS_DEPS):
                depends_cell = v
            elif any(key == k or key in k for key in IO_HEADER_KEYS_STATUS):
                status_cell = v
            elif any(key == k for key in IO_HEADER_KEYS_ORDER):
                order_cell = v
        # 4-col live format `| Step | Section | Tag | Dependency |` has no
        # explicit Deliverable column; the Section cell carries both the
        # `§N` number AND the work-item label ("Calendar App (Recurring
        # Events + ICS Export)"). Use Section text as deliverable when no
        # dedicated column was found.
        if deliverable_cell is None and section_cell:
            deliverable_cell = section_cell
        # Determine n: prefer explicit `§N` in Section cell, else Order cell.
        n = None
        if section_cell:
            m = re.match(r"§?(\d+)", section_cell)
            if m:
                n = int(m.group(1))
        if n is None and order_cell:
            try:
                n = int(order_cell.strip("`"))
            except ValueError:
                pass
        # Parse depends list. Tokens are comma-separated section refs
        # (§-prefixed, optionally with a TNN cross-domain prefix) or the
        # ASCII `--` / `-` placeholder for "no deps". CLAUDE.md No-
        # Unicode-Dashes rule means any TODO using a real em or en dash
        # here is itself a lint violation; we detect ASCII forms only.
        deps = []
        if depends_cell and depends_cell not in {"--", "-", ""}:
            for tok in depends_cell.split(","):
                tok = tok.strip().strip("`").strip()
                if tok:
                    deps.append(tok)
        # Status normalization: extract `[ ]`/`[x]`/`[/]`
        status_norm = ""
        if status_cell:
            m = re.search(r"\[([ x/])\]", status_cell)
            if m:
                status_norm = m.group(1) if m.group(1) != " " else " "
        out.append(
            {
                "n": n,
                "deliverable": deliverable_cell or "",
                "depends_on": deps,
                "status": status_norm,
            }
        )
    return out


def extract_inputs_xrefs(body: str) -> list:
    """Walk under `## Inputs` and return list of XREF refs:
       {target_path: str, target_section: str|None}.

    Surface forms in the wild:
      - bullet form:  `- -> XREF: ...`
      - bullet form (unicode arrow):  `- -> XREF: ...` becomes legal
      - table row:    `| -> XREF: ... | description |`
    All three are graph edges; plain Inputs entries (path-only) are
    consumed-file references and stay out of the graph.
    """
    out = []
    in_inputs = False
    for ln in body.splitlines():
        if ln.startswith("## Inputs"):
            in_inputs = True
            continue
        if in_inputs and ln.startswith("## "):
            break
        if not in_inputs:
            continue
        # Normalize unicode rightwards-arrow (U+2192) to ASCII `->` for
        # match consistency. CLAUDE.md only forbids en/em DASHES; the
        # unicode arrow is allowed and shows up in some Inputs tables.
        normalized = ln.replace("\u2192", "->")
        # Bullet form (existing).
        m = INPUTS_XREF_RE.match(normalized)
        if m:
            target = m.group(1) or m.group(2)
            section = m.group(3)
            out.append({"target_path": target, "target_section": section})
            continue
        # Table-row form: `| -> XREF: <path>[ §N] | ... |` or
        # `| `<path>`[ §N] | ... |` (some files just put the path in a
        # backticked cell). Detect XREF marker and extract the first
        # path-like token.
        if normalized.startswith("|") and "XREF:" in normalized:
            cells = [c.strip() for c in normalized.strip("|").split("|")]
            if cells:
                head = cells[0]
                # Trim arrow + XREF prefix
                head_clean = re.sub(r"^->\s*XREF:\s*", "", head).strip()
                # Path may be wrapped in backticks
                head_clean = head_clean.strip("`")
                # Split into target + optional section
                path_match = re.match(r"^(\S+?)(?:\s+(§\S+))?$", head_clean)
                if path_match:
                    out.append(
                        {
                            "target_path": path_match.group(1),
                            "target_section": path_match.group(2),
                        }
                    )
    return out


def extract_stamps_xrefs(body: str) -> list:
    """Return every `> **Accepted:**` / `> **Deferred:**` stamp XREF ref:
       {kind, severity, target_path, target_section, item_name?}.

    Two-pass parser: first match the stamp header (kind + severity), then
    iterate every XREF clause on the same line. Live stamps in
    `todo/02-kernel-core/TODO-12-native-api-ssdt.md` carry up to 6 XREFs
    on a single Accepted: line; emitting one cache entry per clause keeps
    every deferred-work obligation in the graph instead of dropping all
    but the first.
    """
    out = []
    for ln in body.splitlines():
        header_m = STAMP_HEADER_RE.match(ln)
        if not header_m:
            continue
        kind = header_m.group("kind").lower()
        severity = header_m.group("severity") or ""
        # Iterate every XREF clause from after the header onward.
        for clause in XREF_CLAUSE_RE.finditer(ln, pos=header_m.end()):
            entry = {
                "kind": kind,
                "severity": severity,
                "target_path": clause.group("target_path"),
                "target_section": clause.group("target_section"),
            }
            item = clause.group("item_name")
            if item:
                entry["item_name"] = item
            out.append(entry)
    return out


# --- Node assembly -------------------------------------------------------


def build_node(file_path: Path, repo_root: Path, timestamps: dict, content: str):
    """Construct a single cache node from a TODO file's content. Returns
    (node, errors) where errors is a list of (category, message) tuples
    (file:line is added by the caller). errors == [] on clean parse."""
    try:
        rel = str(file_path.relative_to(repo_root))
    except ValueError:
        rel = file_path.name
    domain = file_path.parent.name
    title_from_h1 = extract_title(content)

    fm, body, fm_err = split_frontmatter(content)
    errors = []
    if fm_err:
        errors.append(("malformed-yaml", fm_err))
        # Continue parsing the body so we still produce a (no-frontmatter)
        # node; the caller decides whether the malformed YAML aborts the
        # whole build. (§2's exit-code rule says "1 on any file's frontmatter
        # malformed"; we honor that at main() level.)
        fm = None

    if fm is not None:
        fm_errors = validate_frontmatter(fm)
        # Treat non-warning categories as errors.
        for cat, msg in fm_errors:
            if cat == "unknown-field":
                # Surface as warning category; main() doesn't fail on these.
                errors.append((cat, msg))
            else:
                errors.append((cat, msg))

    sections_io = extract_implementation_order(body)
    inputs_xrefs = extract_inputs_xrefs(body)
    stamps_xrefs = extract_stamps_xrefs(body)
    section_headings = extract_section_headings(body)

    created_at, last_active_at = timestamps.get(rel, (None, None))

    if fm:
        node = {
            "id": fm.get("id"),
            "schema_version": fm.get("schema_version"),
            "domain": fm.get("domain", domain),
            "status": fm.get("status"),
            "title": fm.get("title", title_from_h1),
            "file_path": rel,
            "created_at": created_at,
            "last_active_at": last_active_at,
        }
        for opt in ("owners", "file_patterns", "depends_on", "satisfies", "superseded_by"):
            if opt in fm:
                node[opt] = fm[opt]
    else:
        # Back-compat: no-frontmatter node. Per the generator spec:
        # id=None, status="no-frontmatter". The validator section will
        # surface these but the generator does not fail on them during
        # the migration window owned by the back-fill section.
        node = {
            "id": None,
            "schema_version": None,
            "domain": domain,
            "status": "no-frontmatter",
            "title": title_from_h1,
            "file_path": rel,
            "created_at": created_at,
            "last_active_at": last_active_at,
        }

    node["sections"] = sections_io
    node["section_headings"] = [{"n": n, "title": t} for n, t in section_headings]
    node["inputs_xrefs"] = inputs_xrefs
    node["stamps_xrefs"] = stamps_xrefs

    return node, errors


# --- main ---------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(
        description="Build the TODO graph cache from todo/**/*.md frontmatter + structure."
    )
    parser.add_argument(
        "--root",
        default="todo",
        help="Root directory to walk (default: todo/)",
    )
    parser.add_argument(
        "--output",
        default="build/todo-cache.json",
        help="Output cache path (default: build/todo-cache.json)",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress per-warning output; only fail-stop and final summary print",
    )
    parser.add_argument(
        "--repo-root",
        default=None,
        help="Repo root (defaults to git toplevel of --root, or its parent)",
    )
    args = parser.parse_args()

    t0 = time.monotonic()

    todo_root = Path(args.root).resolve()
    if not todo_root.is_dir():
        sys.stderr.write(f"[build.py] FATAL: --root {todo_root} is not a directory\n")
        return 1

    if args.repo_root:
        repo_root = Path(args.repo_root).resolve()
    else:
        # Try git toplevel first; fall back to todo_root.parent.
        try:
            res = subprocess.run(
                ["git", "-C", str(todo_root), "rev-parse", "--show-toplevel"],
                capture_output=True,
                text=True,
                check=True,
            )
            repo_root = Path(res.stdout.strip()).resolve()
        except (subprocess.CalledProcessError, FileNotFoundError):
            repo_root = todo_root.parent

    files = walk_todo_files(todo_root)
    if not args.quiet:
        sys.stderr.write(f"[build.py] discovered {len(files)} TODO files under {todo_root}\n")

    timestamps = collect_git_timestamps(repo_root, files)

    nodes = []
    fatal_errors = 0
    warnings = 0

    for f in files:
        # Compute display path: relative to repo_root when possible,
        # otherwise just the basename (test fixtures under /tmp etc.).
        try:
            display_rel = str(f.relative_to(repo_root))
        except ValueError:
            display_rel = f.name
        try:
            content = f.read_text(encoding="utf-8")
        except Exception as exc:
            sys.stderr.write(f"[build.py] FAIL {display_rel}: read error: {exc}\n")
            fatal_errors += 1
            continue

        node, errors = build_node(f, repo_root, timestamps, content)
        rel = display_rel
        for cat, msg in errors:
            if cat == "unknown-field":
                if not args.quiet:
                    sys.stderr.write(f"[build.py] WARN {rel}: {cat}: {msg}\n")
                warnings += 1
            else:
                sys.stderr.write(f"[build.py] FAIL {rel}: {cat}: {msg}\n")
                fatal_errors += 1
        nodes.append(node)

    if fatal_errors > 0:
        sys.stderr.write(
            f"[build.py] FAIL: {fatal_errors} fatal error(s) across "
            f"{len(files)} files; cache NOT written\n"
        )
        return 1

    # Sort nodes by file_path for deterministic output (idempotency).
    nodes.sort(key=lambda n: n["file_path"])

    output_path = Path(args.output).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    # `sort_keys=True` for byte-identical re-runs; trailing newline so the
    # file is shell-friendly (`cat` doesn't show "no newline at end").
    output_path.write_text(
        json.dumps(nodes, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    elapsed = time.monotonic() - t0
    if not args.quiet:
        try:
            display = output_path.relative_to(Path.cwd())
        except ValueError:
            display = output_path
        sys.stderr.write(
            f"[build.py] OK: wrote {len(nodes)} nodes to {display} "
            f"({warnings} warnings, {elapsed:.2f}s)\n"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
