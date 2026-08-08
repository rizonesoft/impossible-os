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
#   3  the corpus moved underneath this run (a TODO changed between being read
#      and the cache being published, or the corpus git history moved). Nothing
#      is written and any previous cache is left byte-identical -- see
#      "THE PRODUCER GENERATION WINDOW" below.
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
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Optional

# The corpus fingerprint rule is SHARED with every reader rather than restated
# here. Two implementations of "has the corpus moved" that can disagree is a
# worse defect than the window this closes, so the producer records exactly what
# `cache_schema._scan_corpus` computes and `cache_schema.check_freshness` reads.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import cache_schema as _cs  # noqa: E402

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
        # DIRECTORIES ONLY are excluded here, and only because the reader's
        # `os.walk` reports them under `dirs` rather than `files` -- so a
        # directory named `TODO-x.md` belongs to neither side's corpus (Codex
        # consistency, section 21, [medium]). A non-regular FILE is deliberately
        # NOT skipped: both sides can see it, so the shared read rule must
        # REFUSE it on both sides rather than let the producer quietly omit what
        # every reader rejects (Codex re-adversarial, section 21, [medium]).
        if p.is_dir():
            continue
        out.append(p)
    return sorted(out)


# --- Git timestamps (batched) -------------------------------------------


class _Provenance(str):
    """A typed provenance marker, distinguishable from a real identity string.

    Subclassing `str` keeps every existing `history_id is None` -> truthiness and
    formatting path working while making `isinstance(p, _Provenance)` the exact
    test for "this is a marker, not an id". A bare string constant would be
    indistinguishable from an identity that happened to equal it.
    """

    __slots__ = ()


# Determinate: no corpus file is under `repo_root`, so no timestamp was derived.
PROVENANCE_NO_DERIVATION = _Provenance("no-derivation")
# Indeterminate: the walk did not answer and we cannot say why. ABORT, do not
# re-ask -- a second probe that succeeds describes a different moment.
PROVENANCE_INDETERMINATE = _Provenance("indeterminate")

# The only answers from `corpus_history_id` that may rescue an INDETERMINATE
# walk. Both are established by its own explicit probes (`rev-parse --git-dir`
# reporting not-a-repository, `rev-parse --verify --quiet HEAD` reporting an
# unborn HEAD), so they are facts about the tree rather than about this moment.
# `no-history:0` is deliberately ABSENT from the INDETERMINATE set: the walk
# produces that itself on a successful empty read, so seeing it only from the
# fallback means the walk failed while git was healthy -- the transient case
# that must abort.
_DETERMINATE_NO_GIT = frozenset(("no-repo", "unborn-head"))

# NO_DERIVATION IS A WEAKER CLAIM AND TAKES A WIDER RESCUE SET (Codex
# adversarial, section 24, [medium]). Applying the INDETERMINATE rule to both
# markers over-refused a legitimate tree: when no corpus file lives under
# `repo_root` NO walk was attempted at all, so `no-history:0` from the fallback
# is not evidence of a failure -- it says the corpus sits in a repository whose
# history has never touched it, and null timestamps are then the honest and
# only possible answer. A REAL `tip:count` is still refused for both markers,
# because that means a history exists which this build did not read.
_DETERMINATE_NO_DERIVATION = _DETERMINATE_NO_GIT | frozenset(("no-history:0",))


def collect_git_timestamps(repo_root: Path, files: list, todo_root=None) -> dict:
    """Return {relative_path_str: (created_at_iso, last_active_at_iso)} for
    every file. One subprocess call walks the entire log so we don't pay
    per-file fork overhead.

    Strategy: `git log --name-only --format=COMMIT %H %ct --reverse` then
    walk the output, recording the FIRST timestamp we see for each path
    (created_at) and continuously updating to the LATEST (last_active_at).
    `--reverse` makes the first-seen=oldest invariant hold without
    sorting per-file.

    Files with no git history (untracked or new) get None timestamps.

    THE WALK REPORTS ITS OWN PROVENANCE. Returning only the timestamps let the
    caller pin the history with a SEPARATE `git log`, which is a different
    generation: move the history to B while this walk resolves, return B-derived
    timestamps, move back to A, and an A-vs-A comparison publishes B's
    timestamps under an A binding (Codex re-adversarial, section 21, [high]).
    The last `COMMIT` line of this very output IS the corpus tip the projection
    came from, and the number of them is its commit count -- the same pair
    `cache_schema.corpus_history_id` computes, verified equal on the live repo
    -- so the provenance costs nothing extra and cannot describe a different
    walk than the one that ran.

    Returns `(timestamps, provenance)`. Provenance is either the walk-derived
    identity string, or one of two typed markers -- and COLLAPSING THOSE TWO INTO
    ONE `None` WAS A HOLE (Codex design review, section 24, [high]). Any `git log`
    failure used to return `None`, and the caller then minted a history id from a
    SECOND `corpus_history_id` probe. A transient git failure that cleared between
    those two calls therefore published null `created_at` / `last_active_at`
    fields under a perfectly valid history binding, which a history-consuming
    reader accepts as fresh. The two states are not the same fact:

      `PROVENANCE_NO_DERIVATION` -- DETERMINATE. No corpus file lives under
      `repo_root`, so no timestamp could be derived from any history and there is
      genuinely nothing for a history check to protect. The caller resolves the
      recorded id from `corpus_history_id`, which fails closed on its own.

      `PROVENANCE_INDETERMINATE` -- the walk did not answer and this process
      cannot say why. The caller must ABORT rather than ask a second time; only
      the determinate sentinels `no-repo` and `unborn-head`, established by
      `corpus_history_id`'s own explicit probes, may rescue it.

    A SUCCESSFUL WALK WITH EMPTY OUTPUT IS DETERMINATE, not a failure: an
    existing HEAD simply has no commit touching the corpus (a tree whose TODO
    files are still untracked is the ordinary case). That yields `no-history:0`,
    which is byte-identical to what `corpus_history_id` returns for the same
    state -- the two must not drift, or the producer's own post-walk comparison
    would reject legitimate builds.

    THE PATHSPEC IS DERIVED FROM `todo_root`, NOT HARDCODED (Codex consistency,
    section 24, [medium]). This walk used a literal `-- todo` while the reader's
    `corpus_history_id` scopes `.` from whatever `--root` it was given, so any
    corpus in a differently-named in-repo directory had the producer recording
    the `todo/` history and the post-walk comparison measuring the OTHER
    directory's -- an rc 3 refusal with nothing concurrent about it. Both sides
    must name the same paths or "the history moved" means nothing.
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

    if not rel_paths:
        # NO FILE IS UNDER `repo_root`, so no timestamp can be derived from its
        # history and the walk below would describe a DIFFERENT tree than the
        # corpus (test fixtures pass `--root /tmp/...` with `--repo-root` at the
        # live repo, which made the provenance the live repo's corpus tip while
        # the corpus itself was not in a repository at all). Provenance None is
        # the honest answer: nothing was derived, so there is nothing for the
        # history binding to protect.
        return {}, PROVENANCE_NO_DERIVATION

    created = {}
    last_active = {}
    # Path-limit the log walk to the corpus directory so runtime scales with
    # its history alone, not total-repo history. Without the pathspec the
    # `--reverse` walk visits every commit in the repo (kernel churn,
    # docs churn, scripts churn) before producing the first TODO
    # timestamp, which makes the 2s budget brittle as the repo grows.
    # Codex quality review caught this: a full-repo scan was the
    # observed bottleneck once commit count crosses ~10x.
    #
    # The pathspec is the corpus's own repo-relative path, so it names exactly
    # what `corpus_history_id` scopes as `.` from the same directory. It falls
    # back to `todo` only when `todo_root` was not supplied or does not sit
    # under `repo_root` -- in which case `rel_paths` is empty and we returned
    # NO_DERIVATION above, so the pathspec is never actually consulted.
    #
    # `--literal-pathspecs` IS LOAD-BEARING NOW THAT THE PATH IS DERIVED (Codex
    # re-adversarial, section 24, [medium]). `--` ends OPTION parsing; it does
    # not stop git interpreting pathspec magic, so a directory legitimately
    # named `:(top)plans` or containing `*` would select something other than
    # itself. While the pathspec was the hardcoded literal `todo` that could not
    # bite; taking it from `--root` makes it caller-controlled. The mismatch is
    # usually a refusal, but if the decoy and the real directory share commits
    # the tip/count can agree while the emitted filenames never match
    # `rel_paths` -- null timestamps that pass the post-walk comparison.
    pathspec = "todo"
    if todo_root is not None:
        try:
            pathspec = Path(todo_root).relative_to(repo_root).as_posix() or "."
        except ValueError:
            pass
    try:
        result = subprocess.run(
            [
                "git",
                "--literal-pathspecs",
                "log",
                "--name-only",
                "--format=COMMIT %H %ct",
                "--reverse",
                "--",
                pathspec,
            ],
            cwd=str(repo_root),
            capture_output=True,
            text=True,
            check=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError, OSError):
        # THE WALK DID NOT ANSWER AND THIS PROCESS CANNOT SAY WHY. `git log`
        # exiting nonzero covers a missing binary, a broken repository, a
        # corrupt ref and a transient failure alike, and none of those is
        # evidence that the corpus has no history. Emitting null timestamps and
        # letting the caller mint an id from a later probe is exactly how a
        # cleared transient failure published a timestamp-less cache under a
        # valid binding (Codex design review, section 24, [high]).
        return {p: (None, None) for p in rel_paths}, PROVENANCE_INDETERMINATE

    cur_ts = None
    walk_commits = []
    for line in result.stdout.splitlines():
        if line.startswith("COMMIT "):
            parts = line.split(" ", 2)
            if len(parts) >= 2:
                walk_commits.append(parts[1])
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
    # `--reverse`, so the LAST commit seen is the newest corpus-touching one.
    tip = walk_commits[-1] if walk_commits else "no-history"
    return out, f"{tip}:{len(walk_commits)}"


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
      malformed-yaml     (handled separately in split_frontmatter; also covers
                          circular YAML alias cycles via _has_yaml_cycle)
      missing-frontmatter: file has no frontmatter block at all. Post-§5
                          (back-fill migration, 2026-04-23) this is FATAL
                          instead of a back-compat `status=no-frontmatter`
                          node.
      unknown-status     : status not in VALID_STATUSES
      invalid-id         : id fails regex
      missing-required   : a REQUIRED_FIELDS entry absent
      schema-version     : schema_version not int OR > SUPPORTED_SCHEMA_VERSION
      forbidden-field    : hand-authored cache-only field (created_at /
                          last_active_at) present
      invalid-field      : optional field present but wrong type/shape
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
    for cache_only in ("created_at", "last_active_at", "stamped_items"):
        if cache_only in fm:
            errors.append(
                (
                    "forbidden-field",
                    f"field '{cache_only}' is auto-derived (cache-only); "
                    "MUST NOT appear in hand-authored frontmatter "
                    "(see docs/infrastructure/todo-metadata.md Auto-Derived Fields). "
                    "stamped_items is emitted by the per-item cache extension only.",
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


# Backtick-quoted symbol form inside a `[x]` item: `foo_init()` -- captures
# `foo_init`. Requires parens on the symbol so prose words in backticks
# (e.g. `INTENTIONAL-STUB`, `[x]`) don't get mis-classified as symbols.
_ITEM_SYMBOL_RE = re.compile(r"`([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*\)`")
# Backtick-quoted file-with-line form: `src/kernel/foo.c:42`. Optional
# leading `./` is stripped. The line number is captured when present.
_ITEM_FILE_LINE_RE = re.compile(
    r"`(?:\./)?([A-Za-z0-9_./-]+\.(?:c|h|asm|S|py|sh|md|json|ps1|bat))(?::(\d+))?`"
)
# Markdown-link form: [label](path/to/file.ext). Path is taken raw; the
# resolver normalizes `../`-relative paths against the TODO file's parent.
_ITEM_MD_LINK_RE = re.compile(
    r"\[[^\]]+\]\(([^)\s]+\.(?:c|h|asm|S|py|sh|md|json|ps1|bat))(?:#[^)]*)?\)"
)


def _normalize_md_link_path(link: str, todo_rel: str) -> str:
    """Resolve a markdown-link path relative to the TODO file's parent dir,
    return repo-rooted POSIX path. Drops fragments. Returns the original
    `link` on failure (kept opaque so the consumer can warn rather than
    silently skip)."""
    try:
        from posixpath import normpath
        todo_dir = "/".join(todo_rel.split("/")[:-1])
        joined = link if link.startswith("/") else (todo_dir + "/" + link)
        return normpath(joined.lstrip("/"))
    except Exception:
        return link


def extract_stamped_items(body: str, todo_rel: str) -> list:
    """Walk each `## N. Title` body for `- [x]` checklist items and emit
    per-item records:

       {section_n, item_idx, item_text, refs: [{kind, file?, symbol?, line?}]}

    Refs combine three surface forms inside the item text:
      - `symbol_name()`         -> {kind: "symbol", symbol, file?}
      - `path/to/file.c:42`     -> {kind: "file",   file, line?}
      - [label](path/to/file.c) -> {kind: "file",   file}

    Symbol refs gain an optional `file` set to the FIRST file ref in the
    SAME item (Codex design review F2: pair within-item file+symbol refs
    so lint Check 7 can resolve symbol-only stamps deterministically).
    Items with no recognizable code refs land with refs: [].

    Refs are deduplicated by full semantic key (Codex Q2). The
    section_n / item_idx pair is stable as long as the section's `[x]`
    item ordering is preserved (1-based section_n, 0-based item_idx).
    """
    out = []
    cur_section = None
    cur_item_idx = 0
    sec_hdr_re = re.compile(r"^## (\d+)\.\s+")
    # Match a `[x]` checklist line. Allow `-` or `*` bullets at any
    # indent; reject blockquoted (`>`) lines because stamp continuations
    # never carry `[x]`.
    item_re = re.compile(r"^\s*[-*]\s*\[x\]\s+(.+?)\s*$")
    for ln in body.splitlines():
        sec_m = sec_hdr_re.match(ln)
        if sec_m:
            cur_section = int(sec_m.group(1))
            cur_item_idx = 0
            continue
        if cur_section is None:
            continue
        item_m = item_re.match(ln)
        if not item_m:
            continue
        text = item_m.group(1)
        file_refs = []
        for fm in _ITEM_FILE_LINE_RE.finditer(text):
            entry = {"kind": "file", "file": fm.group(1)}
            if fm.group(2):
                entry["line"] = int(fm.group(2))
            file_refs.append(entry)
        for lm in _ITEM_MD_LINK_RE.finditer(text):
            link = lm.group(1).strip("`")
            normalized = _normalize_md_link_path(link, todo_rel)
            file_refs.append({"kind": "file", "file": normalized})
        primary_file = file_refs[0]["file"] if file_refs else None
        sym_refs = []
        for sm in _ITEM_SYMBOL_RE.finditer(text):
            entry = {"kind": "symbol", "symbol": sm.group(1)}
            if primary_file:
                entry["file"] = primary_file
            sym_refs.append(entry)
        seen = set()
        merged = []
        for ref in file_refs + sym_refs:
            key = (ref.get("kind"), ref.get("file"), ref.get("symbol"), ref.get("line"))
            if key in seen:
                continue
            seen.add(key)
            merged.append(ref)
        out.append(
            {
                "section_n": cur_section,
                "item_idx": cur_item_idx,
                "item_text": text,
                "refs": merged,
            }
        )
        cur_item_idx += 1
    return out


# Token classes inside an Implementation Order Depends-On cell. A target
# token names a different file (cross-file dep); a section token names a
# section number. Parser walks tokens left-to-right and groups sections
# under whichever target they follow (or "self" for sections at the start).
_DEP_TARGET_RE = re.compile(
    r"^("
    r"D\d{1,2}T\d{1,2}|"   # cross-domain compact: D02T19 (1-digit D also ok)
    r"D\d{1,2}\s*T\d{1,2}|"  # same with space: D02 T19
    r"D\d{1,2}/T\d{1,2}|"    # same with slash: D02/T19 (used by some authors)
    r"D\d{1,2}|"            # cross-domain only (rare): D14
    r"T\d{1,2}|"            # same-domain compact: T17
    r"TODO-\d{1,2}(?:-[a-z0-9-]+)?(?:\.md)?|"  # TODO-NN, TODO-NN-name, .md
    r"\.\.?/\S+|"           # relative path: ./foo or ../bar
    r"\d{2}-[a-z0-9-]+/TODO-\d{1,2}(?:-[a-z0-9-]+)?(?:\.md)?"  # full domain path
    r")$"
)
_DEP_SECTION_RE = re.compile(r"^§(\d+)$")
# Characters authors use as a range separator: ASCII hyphen-minus (0x2D),
# Unicode en dash (U+2013), and Unicode em dash (U+2014). Assembled via
# chr() so the source stays ASCII-dash-only per CLAUDE.md.
_DEP_RANGE_DASHES = "-" + chr(0x2013) + chr(0x2014)
# Range notation shorthand: `section-range` where the separator is any
# dash character above, or a two-dot ellipsis. The second half may or may
# not carry a leading section sign. Authors use this as compact shorthand;
# we expand the sequence so each member passes through _DEP_SECTION_RE.
_DEP_RANGE_RE = re.compile(
    r"^§(\d+)\s*(?:[" + _DEP_RANGE_DASHES + r"]|\.\.)\s*§?(\d+)$"
)
# Squished target+section tokens: a target followed immediately by the
# section sign then a digit, with no separating whitespace. Authors sometimes
# omit the space between the file reference and the section indicator;
# pre-split these so the rest of the tokenizer handles each half normally.
_DEP_SQUISHED_RE = re.compile(
    r"^(?P<target>D\d{1,2}(?:/?T\d{1,2})?|T\d{1,2}|TODO-\d{1,2})§(?P<sec>\d+)$"
)


def _parse_dep_cell(depends_cell: Optional[str]) -> list:
    """Parse one Depends-On cell into a list of {target, sections} groups.
    See extract_implementation_order docstring for grammar + examples."""
    if not depends_cell or depends_cell.strip() in {"--", "-", ""}:
        return []
    # Split on commas first; then for each comma-piece, further split on
    # whitespace so `TODO-08 §8` becomes ["TODO-08", "§8"]. Strip backticks
    # and trailing sentence-level punctuation (`;`, `:`, `)`, `.`). Authors
    # sometimes write prose-style refs like `TCP API from TODO-01;
    # dns_resolve()...`; the trailing `;`/`)`/`.` is not part of the
    # target token and should not reach the resolver.
    raw_tokens: list = []
    for chunk in depends_cell.split(","):
        for tok in chunk.strip().strip("`").split():
            tok = tok.strip().strip("`").strip()
            # Strip both leading and trailing prose punctuation. Authors
            # often write `(TODO-06 §2)` / `;TODO-01` / `TODO-03.` inline
            # in the dep cell; none of the surrounding punctuation is
            # part of the target token and should not reach the resolver.
            tok = tok.lstrip("(")
            tok = tok.rstrip(";:)").rstrip(".")
            if tok:
                raw_tokens.append(tok)
    # Pre-split squished target+section tokens (D02T06 + section + 3 -->
    # D02T06, section+3) so the range/section logic below sees the
    # expected shape. Runs before range expansion because a squished token
    # never contains a range separator.
    split_squished: list = []
    for tok in raw_tokens:
        m = _DEP_SQUISHED_RE.match(tok)
        if m:
            split_squished.append(m.group("target"))
            split_squished.append("§" + m.group("sec"))
        else:
            split_squished.append(tok)
    raw_tokens = split_squished
    # Expand range tokens (`section-range-end`) into the full consecutive
    # list. `§1..§6` or `§1-6` or `§1--§6` all become §1 §2 §3 §4 §5 §6.
    # Degrades gracefully if start > end: emits just the start as a single
    # section so downstream grammar is unchanged.
    expanded: list = []
    for tok in raw_tokens:
        m = _DEP_RANGE_RE.match(tok)
        if m:
            start = int(m.group(1))
            end = int(m.group(2))
            if end < start:
                expanded.append("§" + str(start))
            else:
                for n in range(start, end + 1):
                    expanded.append("§" + str(n))
        else:
            expanded.append(tok)
    raw_tokens = expanded
    # Join bare D<dom> followed by T<num> into a single D<dom>T<num> token
    # so the space-separated form (`D02 T19 §1`) parses identically to the
    # tight form (`D02T19 §1`); TODO-06 §1 documents both as accepted.
    joined: list = []
    i = 0
    while i < len(raw_tokens):
        cur = raw_tokens[i]
        nxt = raw_tokens[i + 1] if i + 1 < len(raw_tokens) else None
        if (
            re.match(r"^D\d{1,2}$", cur)
            and nxt is not None
            and re.match(r"^T\d{1,2}$", nxt)
        ):
            joined.append(cur + nxt)
            i += 2
        else:
            joined.append(cur)
            i += 1
    raw_tokens = joined
    groups: list = []
    current_target = "self"
    current_sections: list = []
    have_seen_anything = False
    for tok in raw_tokens:
        sec_match = _DEP_SECTION_RE.match(tok)
        if sec_match:
            current_sections.append(int(sec_match.group(1)))
            have_seen_anything = True
            continue
        # Anything that looks like a target token starts a new group.
        if _DEP_TARGET_RE.match(tok):
            if have_seen_anything:
                groups.append({"target": current_target, "sections": list(current_sections)})
                current_sections = []
            current_target = tok
            have_seen_anything = True
            continue
        # Unrecognized token: if it looks like prose or a role descriptor
        # (VFS, heap, mutex, (none), atomics, etc.) skip it silently.
        # These are annotation, not file references, and emitting them as
        # opaque targets produces thousands of false-positive stale-xref
        # findings. A malformed file reference that starts with something
        # file-ish (TODO-, D, T, digits, path prefix) will still fail
        # _DEP_TARGET_RE but gets kept below so the validator can flag it.
        looks_file_ish = bool(re.match(
            r"^(?:TODO-|D\d|T\d|\d{2}-|\.{1,2}/)", tok
        ))
        if not looks_file_ish:
            continue
        if have_seen_anything:
            groups.append({"target": current_target, "sections": list(current_sections)})
            current_sections = []
        current_target = tok
        have_seen_anything = True
    if have_seen_anything:
        groups.append({"target": current_target, "sections": list(current_sections)})
    return groups


def extract_implementation_order(body: str) -> list:
    """Walk under `## Implementation Order` and return list of section dicts:
       {n: int|None, deliverable: str, depends_on: [DepGroup], status: str}.

    Each DepGroup is `{target: "self"|"<compact-form>", sections: [int]}`. A
    plain `§1, §2` cell yields `[{target: "self", sections: [1, 2]}]`. A
    cross-file cell like `TODO-08 §8, §9` yields a SINGLE group `[{target:
    "TODO-08", sections: [8, 9]}]` -- the bare `§9` rides on the preceding
    target rather than dangling against the source file (Codex pass 6 H1:
    parsing `TODO-08 §8, §9` as two opaque tokens loses the binding).
    Multi-target cells like `TODO-08 §8, TODO-09 §1` yield two groups.

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
        # Parse depends list into structured groups. A bare cell like
        # `§1, §2` becomes one group `{target: self, sections: [1, 2]}`. A
        # cross-file cell like `TODO-08 §8, §9` becomes one group `{target:
        # TODO-08, sections: [8, 9]}` (the bare `§9` rides on the preceding
        # target). A multi-target cell like `TODO-08 §8, TODO-09 §1` yields
        # two groups. The ASCII `--` / `-` cell is "no deps". CLAUDE.md
        # No-Unicode-Dashes rule means any TODO using a real em or en dash
        # here is itself a lint violation; we detect ASCII forms only.
        deps = _parse_dep_cell(depends_cell)
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
        # `as_posix()`, not `str()`: the WIRE SPELLING of a cache path is
        # POSIX-relative, and `str()` yields the host separator -- so a build on
        # Windows would publish backslash paths that every routed reader then
        # refuses as SHAPE, a cache the producer considers valid and no consumer
        # will read (Codex re-adversarial round 7, section 23 review, [medium]).
        # Fixing it here rather than relaxing the validator keeps one spelling
        # in the artifact instead of two the readers must both accept.
        rel = file_path.relative_to(repo_root).as_posix()
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
        # TODO-06 §5 (back-fill migration) closed 2026-04-23: every
        # TODO-*.md under todo/ now carries a frontmatter block. Missing
        # frontmatter is FATAL -- a file without the block either means
        # the author forgot to run the template OR the file was renamed
        # without updating the block. Hard-fail instead of silently
        # producing an id=None node (which would then ambush downstream
        # stats/backlinks/orphans queries with a mysterious `None` entry).
        errors.append((
            "missing-frontmatter",
            f"{rel}: TODO file has no frontmatter block. "
            "Run `python3 scripts/todo-graph/migrate-add-frontmatter.py "
            f"--root {Path(rel).parent}` or add the block manually -- see "
            "docs/infrastructure/todo-metadata.md for the schema.",
        ))
        # Return a placeholder node so downstream code that expects a
        # dict doesn't crash; main() sees the error and refuses to
        # write the cache.
        node = {
            "id": None,
            "schema_version": None,
            "domain": domain,
            "status": None,
            "title": title_from_h1,
            "file_path": rel,
            "created_at": created_at,
            "last_active_at": last_active_at,
        }

    node["sections"] = sections_io
    node["section_headings"] = [{"n": n, "title": t} for n, t in section_headings]
    node["inputs_xrefs"] = inputs_xrefs
    node["stamps_xrefs"] = stamps_xrefs
    # Per-item stamped_items (per-item cache extension): only emit when the
    # body actually has any `[x]` items; many no-frontmatter / fully-pending
    # TODOs still parse with zero stamped items, in which case the key is
    # omitted to keep the cache compact and the schema's optional contract
    # honest. Lint Check 7 reads stamped_items as the source of truth for
    # stub-behind-stamp findings.
    stamped_items = extract_stamped_items(body, rel)
    if stamped_items:
        node["stamped_items"] = stamped_items

    return node, errors


# --- main ---------------------------------------------------------------


def _publish_atomically(path: Path, blob: bytes) -> None:
    """Write `blob` to `path` so no reader can ever observe a partial file.

    `Path.write_text` truncates in place, so a reader that opened the cache
    mid-write got a prefix of it, and a producer killed mid-write left a
    permanently corrupt one. The temp file is created in the SAME directory so
    `os.replace` is a rename within one filesystem (atomic); fsync before the
    rename so a crash cannot leave the rename durable while the bytes are not.
    """
    fd, tmp = tempfile.mkstemp(dir=str(path.parent), prefix=f".{path.name}.")
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(blob)
            fh.flush()
            os.fsync(fh.fileno())
        # `mkstemp` opens 0600 by design, and `os.replace` PRESERVES the source
        # mode -- so switching from `write_text` to this silently made the cache
        # owner-only. It is a shared build artifact read by lint checks, the
        # snapshot, and CI, so restore the ordinary create mode under the
        # process umask rather than inheriting the temp file's.
        umask = os.umask(0)
        os.umask(umask)
        os.chmod(tmp, 0o666 & ~umask)
        os.replace(tmp, path)
        # FSYNC THE DIRECTORY, not just the file. `os.replace` durability is a
        # property of the DIRECTORY ENTRY: without this a crash could persist
        # the cache's rename while the binding's earlier rename is still only in
        # the page cache, which is exactly the mismatched pair that writing the
        # binding first is meant to make impossible (Codex adversarial, section
        # 21, [high]).
        _fsync_dir(path.parent)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def _fsync_dir(d: Path) -> None:
    """Best-effort directory fsync; not every filesystem supports it."""
    try:
        fd = os.open(str(d), os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)


class _publish_lock:
    """Serialize publish + prune against another producer writing the same cache.

    NOT a repo-wide writer lock -- one advisory `flock` on one lockfile beside
    the cache being published. Deriving the prune's keeper from the cache on
    disk shrank the concurrent-publish race but could not close it, and the
    mtime guard that replaced it was both clock-dependent (in the one change
    whose entire point is removing clock dependence) and wrong on
    coarse-timestamp filesystems (Codex re-adversarial, section 21, [medium]).
    A lock makes "publish the pair, then drop superseded bindings" atomic with
    respect to another build, which is the only thing that was ever needed.

    Best-effort by design: a platform without `flock`, or a `build/` we cannot
    create a lockfile in, must not turn a working build into a failure. The
    unlocked path is exactly the behaviour that shipped before, and its worst
    outcome is a reader REFUSAL that the next rebuild clears.
    """

    def __init__(self, cache_path: Path):
        self._path = cache_path.with_name(f".{cache_path.name}.lock")
        self._fd = None

    def __enter__(self):
        try:
            import fcntl
            self._fd = os.open(str(self._path), os.O_CREAT | os.O_RDWR, 0o666)
            fcntl.flock(self._fd, fcntl.LOCK_EX)
        except (ImportError, OSError):
            if self._fd is not None:
                try:
                    os.close(self._fd)
                except OSError:
                    pass
                self._fd = None
        return self

    def __exit__(self, *exc):
        if self._fd is not None:
            try:
                os.close(self._fd)     # closing the descriptor releases the lock
            except OSError:
                pass
            self._fd = None
        return False


def _prune_stale_bindings(cache_path: Path) -> None:
    """Drop corpus bindings for cache generations that are no longer published.

    Immutable, digest-named bindings would otherwise accumulate one file per
    build. BEST-EFFORT ON PURPOSE: a binding that cannot be removed is garbage,
    never a correctness problem, and failing the build over it would turn a
    successful publish into a refusal after the fact.

    THE KEEPER IS DERIVED FROM THE CACHE ON DISK, NOT FROM WHAT THIS PROCESS
    JUST WROTE (Codex adversarial, section 21, [high]). Two concurrent builds
    interleave -- A publishes, B publishes, A prunes -- and a prune that kept
    "my own binding" would delete B's while B's cache is the one on disk,
    leaving the published cache unreadable until somebody rebuilt. Re-reading
    here means A keeps whatever generation actually won. The remaining window is
    one read plus one unlink rather than the whole publish, and its failure mode
    is a reader REFUSAL, never a wrong answer.
    """
    try:
        # The keeper is derived from the cache ON DISK, inside the same lock
        # that published it, so no other producer can have replaced it between
        # the read and the unlinks. No timestamp is consulted: under the lock
        # there is exactly one live generation, and every other binding is
        # superseded by definition.
        live = _cs.sidecar_path(
            cache_path, hashlib.sha256(cache_path.read_bytes()).hexdigest())
        for p in cache_path.parent.glob(f"{cache_path.name}.corpus-*.json"):
            if p == live:
                continue
            try:
                p.unlink()
            except OSError:
                pass
    except OSError:
        return


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


    nodes = []
    fatal_errors = 0
    warnings = 0
    # THE FINGERPRINT IS TAKEN FROM THE BYTES THIS PASS ACTUALLY CONSUMES, not
    # from a separate scan bracketing it. A pre-scan plus a post-scan proves
    # only that the corpus looked the same at two instants: a file that changed
    # to B, was read as B, and was restored to A before the post-scan compares
    # EQUAL, and the cache is then published with nodes built from B and a
    # binding claiming A (Codex design review, section 21, [high] -- the ABA
    # race). Hashing the exact bytes handed to `build_node` makes the recorded
    # map a statement about what was parsed, so that restoration shows up as the
    # mismatch it is.
    consumed = {}
    # THE RECORDED HISTORY IS THE ONE THE TIMESTAMPS CAME FROM, taken from the
    # walk itself rather than from a second `git log` that could observe a
    # different generation (Codex re-adversarial, section 21, [high]). A None
    # provenance means git could not be consulted, so no timestamp was derived
    # and there is nothing for the history binding to protect; the readers'
    # `corpus_history_id` is then the authority for what to record.
    timestamps, history_id = collect_git_timestamps(repo_root, files, todo_root)
    if isinstance(history_id, _Provenance):
        # NEITHER MARKER MAY BECOME AN IDENTITY BY ITSELF. The fallback probe is
        # allowed to answer only with a DETERMINATE fact about the tree; if it
        # answers with a real `tip:count`, git is healthy NOW and the walk that
        # failed a moment ago was transient -- so the timestamps in hand describe
        # nothing, and publishing them under that id is the exact certification
        # hole this refuses (Codex design review, section 24, [high]).
        try:
            probed = _cs.corpus_history_id(todo_root)
        except _cs.CacheSchemaError as exc:
            sys.stderr.write(f"[build.py] FAIL: {exc}; cache NOT written\n")
            return 3
        allowed = (_DETERMINATE_NO_GIT
                   if history_id is PROVENANCE_INDETERMINATE
                   else _DETERMINATE_NO_DERIVATION)
        if probed not in allowed:
            # THE RULE COVERS BOTH MARKERS, and gating only INDETERMINATE left
            # the same hole one door over (Codex adversarial, section 24,
            # [high]): point `--root` at a corpus that lives in a DIFFERENT
            # repository than `--repo-root` and every path fails
            # `relative_to`, so the walk derives nothing and returns
            # NO_DERIVATION -- while this probe reads the corpus's own
            # repository and hands back a real `tip:count`. Null
            # created_at/last_active_at would then ship under a binding that a
            # history-consuming reader accepts as fresh. Whichever marker came
            # back, a REAL identity here means the corpus has a history this
            # build did not read, and that is never publishable.
            why = ("did not answer, but git is answering now"
                   if history_id is PROVENANCE_INDETERMINATE
                   else "derived no timestamp for any corpus file, yet the "
                        "corpus has git history")
            sys.stderr.write(
                f"[build.py] FAIL: the git log walk that derives "
                f"created_at/last_active_at {why} ({probed}) -- so the "
                f"timestamps in hand describe no history and must not be "
                f"published under it (is --root inside --repo-root?); cache "
                f"NOT written\n")
            return 3
        history_id = probed

    for f in files:
        # Compute display path: relative to repo_root when possible,
        # otherwise just the basename (test fixtures under /tmp etc.).
        try:
            display_rel = str(f.relative_to(repo_root))
        except ValueError:
            display_rel = f.name
        try:
            # THE SHARED RULE, not a private read: a FIFO or broken symlink named
            # TODO-*.md must fail the BUILD, exactly as it fails every reader.
            # Skipping it here published a cache no reader would accept.
            raw_bytes = _cs.read_corpus_file(f)
            content = raw_bytes.decode("utf-8")
        except Exception as exc:
            sys.stderr.write(f"[build.py] FAIL {display_rel}: read error: {exc}\n")
            fatal_errors += 1
            continue
        rel_key = f.relative_to(todo_root).as_posix()
        consumed[rel_key] = hashlib.sha256(raw_bytes).hexdigest()

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

    # ---------------------------------------------------------------------
    # THE PRODUCER GENERATION WINDOW (section 21).
    #
    # Section 17 made both READERS generation-bound. The producer had the
    # mirror-image hole and no reader could ever see it: this process reads a
    # TODO, that file changes, and this process then writes a cache whose NEWER
    # mtime made the already-stale node look FRESH. A freshness test asks "is
    # the cache newer than the corpus?", and the answer was legitimately yes.
    #
    # So the refusal has to live here, at the only point that knows both what
    # was parsed and what is on disk now. Nothing has been written yet, which is
    # what makes "refuse" mean the previous cache survives byte-identical rather
    # than truncated.
    # ---------------------------------------------------------------------
    # AN EMPTY CORPUS IS REFUSED HERE TOO. `check_freshness` already refuses to
    # call a cache fresh against an empty corpus, so a producer that happily
    # published `[]` would emit an artifact every reader rejects -- the two
    # sides of one rule disagreeing (Codex adversarial, section 21, [high]).
    if not consumed:
        sys.stderr.write(
            f"[build.py] FAIL: no TODO files found under {todo_root} -- "
            f"refusing to publish a cache against an empty corpus\n")
        return 3
    try:
        _cs.check_corpus_unchanged(todo_root, consumed)
        moved = _cs.corpus_history_id(todo_root) != history_id
    except _cs.CacheSchemaError as exc:
        sys.stderr.write(
            f"[build.py] FAIL: the TODO corpus moved while this build was "
            f"reading it ({exc}); cache NOT written\n")
        return 3
    if moved:
        sys.stderr.write(
            "[build.py] FAIL: the corpus git history moved while this build "
            "was running, so its created_at/last_active_at fields would not "
            "describe any single generation; cache NOT written\n")
        return 3

    # LEXICAL, NOT RESOLVED, and only the final component matters. `.resolve()`
    # follows a symlink AT `build/todo-cache.json` and `_publish_atomically`
    # then replaces what it POINTS AT -- so `build-and-validate.sh`, the
    # ordinary user-facing path, could overwrite a tracked TODO with cache JSON
    # and its own cleanup would remove the symlink afterwards, hiding what
    # happened. `validate.py` grew an ownership gate for exactly this, but the
    # wrapper runs the producer FIRST, so that gate never saw this path (Codex
    # re-adversarial round 8, section 23 review, [high]).
    #
    # Normalising lexically keeps every equivalent spelling working and still
    # resolves symlinked PARENT directories at open time -- a `build/` symlinked
    # onto a tmpfs is untouched by this. What changes is that `os.replace` now
    # replaces the final directory ENTRY, so `--output X` produces a regular
    # file at X and nothing outside X is ever written.
    output_path = Path(os.path.abspath(os.path.normpath(args.output)))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    # `sort_keys=True` for byte-identical re-runs; trailing newline so the
    # file is shell-friendly (`cat` doesn't show "no newline at end").
    blob = (json.dumps(nodes, indent=2, sort_keys=True) + "\n").encode("utf-8")
    cache_sha = hashlib.sha256(blob).hexdigest()

    # BINDING FIRST, CACHE SECOND, BOTH ATOMIC -- and the binding's NAME carries
    # the cache digest, so the pair commits without a two-file transaction
    # (Codex design review, section 21, [medium]). Die anywhere in here and the
    # durable state is still a cache sitting next to the binding that describes
    # it: before the replace that is the OLD pair, after it the NEW one. A
    # fixed-name sidecar could not do that -- it would leave new-cache with
    # old-binding, which every reader refuses until somebody rebuilds.
    side = _cs.sidecar_path(output_path, cache_sha)
    binding = (json.dumps({
        "schema": _cs.SIDECAR_SCHEMA,
        "cache_sha256": cache_sha,
        "history_id": history_id,
        "corpus": consumed,
    }, indent=2, sort_keys=True) + "\n").encode("utf-8")
    with _publish_lock(output_path):
        _publish_atomically(side, binding)
        _publish_atomically(output_path, blob)
        _prune_stale_bindings(output_path)

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
