#!/usr/bin/env python3
# ============================================================================
# scripts/todo-graph/validate.py -- TODO graph integrity validator
#
# Owner: TODO-06 §3 (Validator) in todo/00-infrastructure/.
# Consumer of: build/todo-cache.json (produced by scripts/todo-graph/build.py).
#
# Runs eight graph-integrity checks on the cache and the live TODO files.
# Each check is independent; failures are tagged by category and printed in
# the t_pass / t_fail style used by scripts/test-tooling.sh. Exit 0 iff
# zero failures; exit 1 otherwise so CI (TODO-06 §6) can gate on it.
#
#   1. Stale XREF: every depends_on / satisfies / Inputs XREF / Accepted+
#      Deferred XREF must resolve to an existing id-or-file in the cache.
#   2. Dangling section ref: every §N inside an Implementation Order
#      Depends-On cell must resolve to a real `## N.` heading in the
#      target file (catches renumbering drift).
#   3. Orphaned IO row: every Implementation Order row needs a matching
#      `## N.` body section, and vice versa. Drift on either side fails.
#   4. Dependency cycle: build the depends_on digraph from frontmatter
#      and run DFS; report any cycle by printing the cycle path.
#   5. Test-runner bat alignment: every `**Test runner:**` line that
#      names a `scripts\debug\<layer>\run-<name>-tests.bat` path must
#      resolve to a real file on disk. N/A / No-test-surface sentinels
#      are exempt.
#   6. Status-transition audit: status: done requires every IO row [x];
#      status: draft must NOT have all-rows-[x] (promote to done).
#   7. $schema reachability: per-file frontmatter `$schema` must point
#      at an existing file (skipped when the field is absent).
#   8. Duplicate id: frontmatter `id` must be globally unique across
#      the todo tree. Added 2026-04-23 (§6) to catch rename-into-
#      collision PRs that build_id_index would otherwise mask.
#
# CLI:
#   validate.py [--cache PATH] [--quiet] [--warnings-only]
#               [--fix-line-numbers [--write]]
#
# --warnings-only:    downgrade check-1 stale-XREF FAILs to WARNs during
#                     migration (frontmatter back-fill is §5; before then,
#                     every XREF resolves via filename instead of id).
# --fix-line-numbers: re-resolve every `(item: "NAME" at line N)`
#                     parenthetical to the current line of the named
#                     item. By default DRY-RUN: prints proposed rewrites
#                     and exits without touching files. Add --write to
#                     actually rewrite. FAILs if the named item resolves
#                     to multiple lines (no first-match-wins; Codex pass
#                     6 M1: ambiguity must surface, not be hidden).
#
# Performance: runs in <2s on the live 223-file tree. All file reads are
# done once at startup (single in-memory snapshot per run; Codex pass 6 M1
# atomic-snapshot contract) so cache-based and direct-reread checks
# observe the same repo state.
# ============================================================================

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Optional

# Sibling module, stdlib-only and side-effect-free at import. It owns the
# `(domain, number)` derivation `build_path_index` files nodes under, so the
# cache validator and this resolver cannot disagree about what collides.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import cache_schema  # noqa: E402


# --- Constants -----------------------------------------------------------

VALID_TEST_LAYERS = ("kernel", "usermode", "desktop", "release")

# `**Test runner:**` line variants. We accept both the canonical
# `> **Test runner:** ` blockquote form and the un-blockquoted form, with
# an optional backtick-quoted bat path. The path uses Windows-style
# backslashes per the project convention.
TEST_RUNNER_LINE_RE = re.compile(
    r"^\s*>?\s*\*\*Test runner:\*\*\s*(?P<rest>.+)$"
)
TEST_RUNNER_BAT_RE = re.compile(
    # Backtick-quoted path under scripts/debug/<layer>/run-<name>-tests.bat
    # OR run-all-<layer>-tests.bat. Accepts BOTH `\` (Windows-canonical per
    # the project convention) AND `/` separators since both forms appear in
    # tracked TODOs. Tightened (Codex consistency review): the previous
    # `run-<anything>.bat` shape accidentally accepted launcher bats
    # (run-to-vhdx.bat, run-to-iso.bat, run-write-usb.bat) as test
    # runners, eroding the intended distinction between non-destructive
    # test runners and per-artifact launchers (the destructive USB writer
    # in particular). Now requires the trailing `-tests` segment.
    r"`scripts[\\/]+debug[\\/]+(?P<layer>[a-z]+)[\\/]+(?P<bat>run-[a-z0-9_-]+-tests\.bat)`",
    re.IGNORECASE,
)

# `(item: "NAME" at line N)` parenthetical inside Accepted/Deferred stamps.
ITEM_LINE_RE = re.compile(r'\(item:\s*"(?P<name>[^"]+)"\s+at\s+line\s+(?P<n>\d+)\)')

# Stamp body matcher: pulls everything after `> **Accepted:**` or
# `> **Deferred:**` so we can locate the `(item: ... at line ...)` clause
# AND the XREF target on the same line. Used by --fix-line-numbers.
STAMP_LINE_RE = re.compile(
    r"^\s*>\s*\*\*(?P<kind>Accepted|Deferred):\*\*\s+(?P<rest>.+)$"
)

# Resolver patterns mirror build.py's _DEP_TARGET_RE so the validator and
# parser agree on what a target token looks like.
COMPACT_DT_RE = re.compile(r"^D(?P<dom>\d{2})T(?P<num>\d{1,2})$")
COMPACT_T_RE = re.compile(r"^T(?P<num>\d{1,2})$")
COMPACT_D_RE = re.compile(r"^D(?P<dom>\d{2})$")
DOMAIN_PATH_RE = re.compile(r"^(?P<dom>\d{2})-[a-z0-9-]+/TODO-(?P<num>\d{1,2})$")
TODO_NN_RE = re.compile(r"^TODO-(?P<num>\d{1,2})$")
TODO_FILENAME_RE = re.compile(r"^TODO-(?P<num>\d{1,2})-")


# --- Finding type --------------------------------------------------------

class Finding:
    __slots__ = ("check", "file", "detail", "severity")

    def __init__(self, check: str, file: str, detail: str, severity: str = "FAIL"):
        self.check = check
        self.file = file
        self.detail = detail
        self.severity = severity

    def format(self) -> str:
        return f"[{self.severity}] {self.check}: {self.file}: {self.detail}"


# --- Cache loader --------------------------------------------------------

def load_or_rebuild_cache(cache_path: Path, repo_root: Path, quiet: bool) -> list:
    """Load build/todo-cache.json. If absent, invoke build.py to regenerate.
    If the cache was produced by an older build.py (opaque-string
    depends_on instead of dict-shape {target, sections[int]}), force a
    rebuild instead of crashing downstream with AttributeError when the
    validator dereferences grp.get(...). Codex pass 9 M1."""
    need_rebuild = not cache_path.exists()
    if not need_rebuild:
        try:
            nodes = json.loads(cache_path.read_text(encoding="utf-8"))
            if _cache_shape_is_stale(nodes):
                if not quiet:
                    sys.stderr.write(
                        f"[validate.py] cache at {cache_path} is stale (pre-pass-6 "
                        "sections[].depends_on shape); rebuilding via build.py\n"
                    )
                need_rebuild = True
        except (OSError, json.JSONDecodeError) as exc:
            if not quiet:
                sys.stderr.write(f"[validate.py] cache at {cache_path} unreadable ({exc}); rebuilding\n")
            need_rebuild = True
    if need_rebuild:
        if not cache_path.exists() and not quiet:
            sys.stderr.write(f"[validate.py] cache absent at {cache_path}; rebuilding via build.py\n")
        # Locate build.py via validate.py's own directory (they're siblings);
        # don't assume repo_root/scripts/todo-graph/ since --repo-root for
        # regression fixtures points at a synthetic tree.
        build_py = Path(__file__).resolve().parent / "build.py"
        if not build_py.exists():
            sys.stderr.write(f"[validate.py] FATAL: build.py not found next to validate.py ({build_py})\n")
            sys.exit(2)
        result = subprocess.run(
            [sys.executable, str(build_py),
             "--quiet", "--output", str(cache_path),
             "--root", str(repo_root / "todo"),
             "--repo-root", str(repo_root)],
            cwd=str(repo_root),
        )
        if result.returncode != 0:
            sys.stderr.write(f"[validate.py] FATAL: build.py failed (exit {result.returncode})\n")
            sys.exit(2)
    return json.loads(cache_path.read_text(encoding="utf-8"))


def _cache_shape_is_stale(nodes: list) -> bool:
    """Return True when sections[].depends_on still carries the pre-pass-6
    opaque-string shape instead of the expected {target, sections[]} dict
    shape. A non-empty dep-group that is a plain string is the tell.

    Scans the ENTIRE cache (Codex pass 10 M1): previously this returned
    False on the first dict-shaped entry seen, which mis-classified a
    mixed cache (some migrated rows, some legacy rows) as fresh. Any
    legacy string entry anywhere in the cache now forces a rebuild.

    Robust against malformed caches (non-dict nodes / non-dict sections):
    they don't prove anything about the shape, so skip them instead of
    crashing. Downstream Ctx._validate_nodes drops malformed rows."""
    for n in nodes or []:
        if not isinstance(n, dict):
            continue
        sections = n.get("sections")
        if not isinstance(sections, list):
            continue
        for s in sections:
            if not isinstance(s, dict):
                continue
            deps = s.get("depends_on")
            if not isinstance(deps, list):
                continue
            for grp in deps:
                if not isinstance(grp, dict):
                    return True
    return False


def load_file_snapshot(repo_root: Path, nodes: list) -> dict:
    """Read every TODO file's text ONCE at startup and return a mapping
    file_path -> text. Single in-memory snapshot per run so all checks (and
    --fix-line-numbers, if enabled) observe identical repo state. Codex
    pass 6 M1 atomic-snapshot contract."""
    snapshot = {}
    for n in nodes:
        rel = n.get("file_path")
        if not rel:
            continue
        full = repo_root / rel
        try:
            snapshot[rel] = full.read_text(encoding="utf-8")
        except (FileNotFoundError, OSError) as exc:
            sys.stderr.write(f"[validate.py] WARN: cannot read {rel}: {exc}\n")
            snapshot[rel] = ""
    return snapshot


# --- XREF resolver -------------------------------------------------------

def build_id_index(nodes: list) -> dict:
    """Return {id: file_path} for every node carrying a non-null id. Used
    by check 1 (stale-XREF) to look up frontmatter id references."""
    return {n["id"]: n["file_path"] for n in nodes if n.get("id")}


def build_path_index(nodes: list) -> dict:
    """Return {(domain, todo_number): file_path} for every node, plus
    {filename_stem: file_path} for unambiguous filename references.
    Used by the resolver below for compact-form XREFs."""
    by_dn = {}
    by_filename = {}
    dn_collisions = set()
    stem_collisions = set()
    stem_paths = {}
    for n in nodes:
        rel = n.get("file_path")
        if not rel:
            continue
        # path like 02-kernel-core/TODO-19-foo.md
        #
        # ONE DERIVATION, SHARED WITH THE CACHE VALIDATOR (section 22).
        # `cache_schema` refuses a cache in which two nodes land on the same
        # `(domain, number)` slot, because whichever one this index kept would
        # silently rebind every edge naming the other. That refusal is sound
        # only if it computes the SAME key this index does, and two copies
        # drifted immediately: this hardcoded the prefixes `00-` through `18-`
        # while the validator matched any two digits, so a legitimate domain-19
        # TODO went unindexed here and keyed there. A hardcoded domain list is
        # a maintenance trap besides -- it stops indexing the day a 19th domain
        # is added, with nothing reporting it.
        key = cache_schema.resolver_key(rel)
        if key is not None:
            if key in by_dn:
                dn_collisions.add(key)
            by_dn[key] = rel
        # filename stem (TODO-19-foo)
        stem = Path(rel).stem
        if stem in by_filename:
            stem_collisions.add(stem)
        by_filename[stem] = rel
        # EVERY candidate, not just the survivor. `by_filename` keeps its
        # flattened shape so existing consumers are untouched, but a resolver
        # branch that must reason about ambiguity cannot do it from a map that
        # already discarded the losers -- the letter-numbered branch below
        # scanned `by_filename.values()` and saw ONE path for two files.
        stem_paths.setdefault(stem, []).append(rel)
    # COLLISION-AWARE, NOT FLATTENED (section 22). Both maps let a later node
    # overwrite an earlier one and returned only the survivor, so a reference
    # naming the LOSER resolved to the WINNER -- silently, at a normal exit
    # code. Reproduced with `todo/19-x/TODO-01-first.md` and `TODO-01-second.md`:
    # an explicit XREF to `19-x/TODO-01-first.md` resolved to `...-second.md`
    # while `check_stale_xref` reported nothing, so the canonical
    # build-and-validate gate accepted a graph that was already wrong.
    # Recording the ambiguity lets `resolve_xref_target` refuse rather than
    # guess, which downgrades a WRONG BINDING to an ordinary unresolved-XREF
    # finding -- visible, and at the right exit code (Codex adversarial,
    # section 22 round 5, [high]).
    return {"by_dn": by_dn, "by_filename": by_filename,
            "dn_collisions": dn_collisions, "stem_collisions": stem_collisions,
            "stem_paths": stem_paths}


def _dn_lookup(path_index: dict, key) -> Optional[str]:
    """Resolve a `(domain, number)` key, refusing an AMBIGUOUS one.

    Returning the survivor of an overwrite is a WRONG ANSWER dressed as a
    right one: the reference named a specific file and got a different one,
    with nothing reporting it. None makes it an ordinary unresolved XREF,
    which every caller already handles and which the validator counts
    (section 22)."""
    if key in path_index.get("dn_collisions", ()):
        return None
    return path_index["by_dn"].get(key)


def _stem_lookup(path_index: dict, stem: str) -> Optional[str]:
    """Resolve a bare filename stem, refusing an ambiguous one. Same rule and
    same reason as `_dn_lookup`; duplicate stems across domains are a
    supported layout, so only the REFERENCE is refused, never the corpus."""
    if stem in path_index.get("stem_collisions", ()):
        return None
    return path_index["by_filename"].get(stem)


def _all_paths(path_index: dict) -> set:
    """Every file_path the index knows, from ALL stem candidates.

    `by_filename.values()` holds only the LAST path per stem, so an exact
    path-membership test against it is decided by cache row order: with
    duplicate supported stems in two domains, `../01-a/TODO-A-Shared.md`
    returned None while `../02-b/TODO-A-Shared.md` resolved, purely because the
    latter was inserted second. That is a FALSE stale-XREF finding on a
    perfectly good reference (Codex adversarial, section 22 round 6, [medium]).
    """
    paths = set(path_index.get("by_filename", {}).values())
    for candidates in path_index.get("stem_paths", {}).values():
        paths.update(candidates)
    return paths


def resolve_xref_target(target: str, source_file: str, id_index: dict, path_index: dict) -> Optional[str]:
    """Resolve a target token (T17, D02T19, D14, TODO-08, 02-kernel-core/TODO-19,
    full filename with or without .md, ./ or ../ relative paths, optional
    #anchor fragments, or a frontmatter id) to a cache file_path. Returns
    None when no resolution succeeds.

    The same-domain `T<num>` form is resolved relative to source_file's
    domain folder; absent that, no resolution (do NOT scan all domains for
    a same-domain compact form -- the spec wants the author to use the
    cross-domain `D<dom>T<num>` form when crossing folders)."""
    if not target:
        return None
    target = target.strip().strip("`").strip()
    # Strip markdown anchor fragments (#section-heading) BEFORE resolution.
    # Anchors target subsections on GitHub; they are never part of a file id
    # or filename. Applying this up front lets every resolution step below
    # work uniformly whether the caller passed a fragment or not.
    if "#" in target:
        target = target.split("#", 1)[0]
    # Drop trailing slash that some authors include on path-form XREFs.
    target = target.rstrip("/")
    if not target:
        return None
    # Normalize several compact-form variants into the canonical D<dd>T<nn>
    # shape used by COMPACT_DT_RE:
    #   - D<d>/T<n>     (slash-separator)
    #   - D<d> T<n>     (space-separator, already pre-joined by _parse_dep_cell)
    #   - D<d>/TODO-<n> (hybrid shorthand used by some authors)
    #   - D<d>          (1-digit domain, zero-padded)
    slash_m = re.match(r"^D(\d{1,2})/T(\d{1,2})$", target)
    if slash_m:
        target = f"D{int(slash_m.group(1)):02d}T{int(slash_m.group(2))}"
    else:
        hybrid_m = re.match(r"^D(\d{1,2})/TODO-(\d{1,2})$", target)
        if hybrid_m:
            target = f"D{int(hybrid_m.group(1)):02d}T{int(hybrid_m.group(2))}"
        else:
            pad_m = re.match(r"^D(\d)(T\d{1,2})?$", target)
            if pad_m:
                target = f"D{int(pad_m.group(1)):02d}" + (pad_m.group(2) or "")
    # 1. Frontmatter id (post-§5 migration)
    if target in id_index:
        return id_index[target]
    # 2. Cross-domain compact: D02T19
    m = COMPACT_DT_RE.match(target)
    if m:
        dom, num = m.group("dom"), int(m.group("num"))
        return _dn_lookup(path_index, (dom, num))
    # 3. Same-domain compact: T17 (resolved against source_file's domain)
    m = COMPACT_T_RE.match(target)
    if m:
        src_parts = Path(source_file).parts
        if len(src_parts) >= 2:
            src_dom = src_parts[-2][:2]
            return _dn_lookup(path_index, (src_dom, int(m.group("num"))))
        return None
    # 3a. Domain-only compact: D14 -- resolve to the domain's INDEX.md.
    # Authors use `D14` to reference an entire domain's scope; the INDEX.md
    # is the canonical landing page. Must come after D<n>T<n> (step 2) so
    # `D02T19` doesn't match the D-only branch.
    m = COMPACT_D_RE.match(target)
    if m:
        dom = m.group("dom")
        # Find any file in this domain, walk back to the INDEX.md sibling.
        for (d, _n), fp in path_index["by_dn"].items():
            if d == dom:
                idx = str(Path(fp).parent / "INDEX.md")
                return idx
        return None
    # 4. Full domain path: 02-kernel-core/TODO-19 (with or without -name, .md)
    m = DOMAIN_PATH_RE.match(target)
    if m:
        dom, num = m.group("dom"), int(m.group("num"))
        return _dn_lookup(path_index, (dom, num))
    # 4a. Full domain path with -name[.md] suffix: 02-kernel-core/TODO-19-foo
    # or 02-kernel-core/TODO-19-foo.md. Common surface form in Inputs XREFs
    # and stamp links. Extract domain + TODO number, resolve via by_dn.
    m = re.match(r"^(?P<dom>\d{2})-[a-z0-9-]+/TODO-(?P<num>\d{1,2})-[a-z0-9-]+(?:\.md)?$", target)
    if m:
        dom, num = m.group("dom"), int(m.group("num"))
        return _dn_lookup(path_index, (dom, num))
    # 4b. Domain-only reference (`05-storage-filesystems`, with or without a
    # trailing `/INDEX.md`). Authors use this to mean "the whole domain"
    # when the XREF is scope-level rather than file-level; resolve to the
    # domain's INDEX.md as the canonical landing page.
    m = re.match(r"^(?P<dom>\d{2})-[a-z0-9-]+(?:/INDEX\.md)?$", target)
    if m:
        dom = m.group("dom")
        for (d, _n), fp in path_index["by_dn"].items():
            if d == dom:
                return str(Path(fp).parent / "INDEX.md")
        return None
    # 4c. Letter-numbered TODO file (TODO-A, TODO-A-Win32k-Shadow-...).
    # A small set of catalog/master-table files use a letter suffix instead
    # of a numeric one. Match by filename stem or full domain path form.
    letter_m = re.match(
        r"^(?:(?P<dom>\d{2})-[a-z0-9-]+/)?TODO-(?P<letter>[A-Z])(?:-[A-Za-z0-9-]+)?(?:\.md)?$",
        target,
    )
    if letter_m:
        # Scan by_filename values for a matching path tail.
        # EXACT STEM FIRST. This branch keys on the LETTER alone and throws the
        # rest of the stem away, so `TODO-A-user32-export-master-table` -- which
        # names exactly one file -- was matched against every `TODO-A-*` in the
        # tree. With three of them in the corpus that produced an arbitrary
        # answer before, and an over-refusal once ambiguity started being
        # refused. A fully-spelled stem is not ambiguous and must resolve.
        exact = letter_m.group(0)
        if exact.endswith(".md"):
            exact = exact[:-3]
        exact = exact.rsplit("/", 1)[-1]
        exact_paths = path_index.get("stem_paths", {}).get(exact)
        if exact_paths and letter_m.group("dom"):
            # A domain qualifier narrows the candidates BEFORE the uniqueness
            # test -- otherwise `01-a/TODO-A-Shared.md` is refused as ambiguous
            # on the strength of a same-named file in another domain, which is
            # exactly the reference form that disambiguates it.
            want = letter_m.group("dom")
            exact_paths = [fp for fp in exact_paths
                           if fp.startswith("todo/")
                           and fp.split("/")[1][:2] == want]
        # A SPELLED-OUT TARGET IS DECIDED BY ITS EXACT MATCH COUNT, ZERO
        # INCLUDED. Only the literal `TODO-A` / `TODO-A.md` may reach the coarse
        # letter scan below. Letting a spelled target fall through meant a
        # MISSING one silently bound to a different file -- `TODO-A-Missing.md`
        # resolved to `todo/19-x/TODO-A-Actual.md`, because that scan matches on
        # the letter alone -- so a typo, a rename or a deleted target redirected
        # every XREF naming it while `check_stale_xref` reported nothing (Codex
        # adversarial, section 22 round 6, [high]).
        if exact != f"TODO-{letter_m.group('letter')}":
            if exact_paths and len(exact_paths) == 1:
                return exact_paths[0]
            return None
        if exact_paths:
            return exact_paths[0] if len(exact_paths) == 1 else None
        stem_fragment = f"TODO-{letter_m.group('letter')}"
        # COLLECT ALL MATCHES, then refuse if more than one. This scanned and
        # returned the FIRST hit from a `set()` -- so with two files carrying
        # the same letter stem the answer depended on set iteration order, and
        # a reference naming one silently bound to the other. It is the same
        # overwrite class as the flattened indexes, in the one branch that does
        # not consult them (Codex adversarial, section 22 round 5, [medium]).
        want_dom = letter_m.group("dom")
        matches = []
        for fp in sorted({q for ps in path_index.get("stem_paths", {}).values()
                           for q in ps} or _all_paths(path_index)):
            tail = fp.rsplit("/", 1)[-1]
            if not (tail.startswith(stem_fragment + "-")
                    or tail == stem_fragment + ".md"):
                continue
            have_dom = fp.split("/")[1][:2] if fp.startswith("todo/") else None
            if want_dom is None or want_dom == have_dom:
                matches.append(fp)
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1 and want_dom is None:
            # SAME-DOMAIN WINS, exactly as bare `TODO-NN` resolves against the
            # source file's domain in branch 5. Three `TODO-A-*` files exist
            # across different domains, so an unqualified `TODO-A` was binding
            # to whichever one `set()` iteration yielded first -- arbitrary, and
            # silently so. Preferring the source's own domain makes the common
            # in-domain reference deterministic; anything else stays ambiguous
            # and surfaces as an unresolved XREF the author must qualify.
            src_parts = Path(source_file).parts
            src_dom = src_parts[-2][:2] if len(src_parts) >= 2 else None
            same = [fp for fp in matches
                    if fp.startswith("todo/") and fp.split("/")[1][:2] == src_dom]
            if len(same) == 1:
                return same[0]
        # Zero matches, or an ambiguous reference the author must qualify.
        return None
    # 5. Bare TODO-NN (resolved against source_file's domain)
    m = TODO_NN_RE.match(target)
    if m:
        src_parts = Path(source_file).parts
        if len(src_parts) >= 2:
            src_dom = src_parts[-2][:2]
            return _dn_lookup(path_index, (src_dom, int(m.group("num"))))
        return None
    # 5a. TODO-NN-name[.md] (same-domain filename with slug and/or .md).
    # Resolves against source_file's domain since no domain prefix is given.
    m = re.match(r"^TODO-(?P<num>\d{1,2})-[a-z0-9-]+(?:\.md)?$", target)
    if m:
        src_parts = Path(source_file).parts
        if len(src_parts) >= 2:
            src_dom = src_parts[-2][:2]
            return _dn_lookup(path_index, (src_dom, int(m.group("num"))))
        return None
    # 6. Filename stem (TODO-19-foo) -- exact match against any domain
    if target in path_index["by_filename"]:
        return _stem_lookup(path_index, target)
    # 7. Filename stem with .md suffix
    if target.endswith(".md"):
        stem = target[:-3]
        if stem in path_index["by_filename"]:
            return _stem_lookup(path_index, stem)
    # 8. Relative-path forms (./TODO-..., ../NN-dom/TODO-..., etc.).
    # Normalize against source_file's directory using filesystem semantics,
    # then match the normalized path against the cache. Works for any depth
    # of `..` traversal without having to enumerate surface forms.
    if target.startswith(("./", "../")) or "/" in target:
        src_dir = Path(source_file).parent
        try:
            resolved = (src_dir / target).resolve(strict=False)
            # Cache paths are stored relative to the repo root (todo/...),
            # so rebuild a relative form starting at `todo/`.
            parts = resolved.parts
            if "todo" in parts:
                idx = parts.index("todo")
                rel_path = "/".join(parts[idx:])
                if rel_path in _all_paths(path_index):
                    return rel_path
                # INDEX.md relative forms (../07-networking/INDEX.md).
                # Return the normalized INDEX.md path if it names a live
                # domain folder.
                if rel_path.endswith("/INDEX.md"):
                    dom_dir = rel_path.split("/")[1]
                    dom_m = re.match(r"^(\d{2})-", dom_dir)
                    if dom_m:
                        dom = dom_m.group("dom") if False else dom_m.group(1)
                        for (d, _n), fp in path_index["by_dn"].items():
                            if d == dom:
                                return rel_path
                # Path does not include the slug or .md suffix: try to map
                # the relative form back to (domain, num) and look up in
                # by_dn. Handles `../02-kernel-core/TODO-03` (no slug, no
                # .md). Normalized `rel_path` would be something like
                # `todo/02-kernel-core/TODO-03`.
                tail = rel_path.rsplit("/", 1)[-1]
                rel_parts = rel_path.split("/")
                if len(rel_parts) >= 3 and rel_parts[0] == "todo":
                    dom_dir = rel_parts[1]
                    dom_m = re.match(r"^(\d{2})-", dom_dir)
                    num_m = re.match(r"^TODO-(\d{1,2})(?:-|\.md|$)", tail)
                    if dom_m and num_m:
                        return _dn_lookup(path_index, 
                            (dom_m.group(1), int(num_m.group(1)))
                        )
        except (OSError, ValueError):
            pass
    # 9. Already a path-like form (00-domain/TODO-NN-name.md).
    if target in _all_paths(path_index):
        return target
    # No resolution.
    return None


# --- Check 1: Stale XREF -------------------------------------------------

def check_stale_xref(nodes: list, id_index: dict, path_index: dict, warnings_only: bool) -> list:
    findings: list = []
    sev_xref = "WARN" if warnings_only else "FAIL"
    for n in nodes:
        rel = n["file_path"]
        # Frontmatter depends_on / satisfies (resolve via id only)
        for field in ("depends_on", "satisfies"):
            for ref in n.get(field, []) or []:
                if ref not in id_index:
                    findings.append(Finding(
                        "stale-xref", rel,
                        f"frontmatter {field}: '{ref}' does not resolve to any TODO id",
                        severity=sev_xref,
                    ))
        # Inputs XREFs (resolve via target_path; covers compact + path forms)
        for x in n.get("inputs_xrefs", []) or []:
            tp = x.get("target_path")
            if not tp:
                continue
            if resolve_xref_target(tp, rel, id_index, path_index) is None:
                findings.append(Finding(
                    "stale-xref", rel,
                    f"Inputs XREF target '{tp}' does not resolve to any TODO file",
                    severity=sev_xref,
                ))
        # Accepted/Deferred stamp XREFs
        for x in n.get("stamps_xrefs", []) or []:
            tp = x.get("target_path")
            if not tp:
                continue
            if resolve_xref_target(tp, rel, id_index, path_index) is None:
                kind = x.get("kind", "stamp")
                findings.append(Finding(
                    "stale-xref", rel,
                    f"{kind} XREF target '{tp}' does not resolve to any TODO file",
                    severity=sev_xref,
                ))
        # Implementation Order Depends-On cross-file targets (Codex pass 7 H1).
        # check_dangling_section_ref skips unresolved targets so the bare
        # broken-target case (e.g. `Depends On: TODO-99 §1` where TODO-99
        # doesn't exist) needs catching here. Self-targets are skipped
        # since the source file always resolves to itself.
        for sec in n.get("sections", []) or []:
            for grp in sec.get("depends_on", []) or []:
                target = grp.get("target")
                if not target or target == "self":
                    continue
                if resolve_xref_target(target, rel, id_index, path_index) is None:
                    findings.append(Finding(
                        "stale-xref", rel,
                        f"IO row {sec.get('n')} Depends-On target '{target}' "
                        "does not resolve to any TODO file",
                        severity=sev_xref,
                    ))
    return findings


# --- Check 2: Dangling section ref ---------------------------------------

def check_dangling_section_ref(nodes: list, id_index: dict, path_index: dict) -> list:
    """Every §N in an Implementation Order Depends-On group must resolve to
    a real `## N.` heading in the target file (self by default; the dep
    parser splits cross-file groups so the binding is preserved)."""
    findings: list = []
    headings_by_file: dict = {n["file_path"]: {h["n"] for h in n.get("section_headings", [])}
                              for n in nodes}
    for n in nodes:
        rel = n["file_path"]
        for sec in n.get("sections", []) or []:
            for grp in sec.get("depends_on", []) or []:
                target = grp.get("target", "self")
                sections = grp.get("sections", []) or []
                if target == "self":
                    target_file = rel
                else:
                    target_file = resolve_xref_target(target, rel, id_index, path_index)
                if target_file is None:
                    # check_stale_xref will report the unresolved target;
                    # check 2 only flags missing §N in resolved files.
                    continue
                target_headings = headings_by_file.get(target_file, set())
                for sn in sections:
                    if sn not in target_headings:
                        findings.append(Finding(
                            "dangling-section", rel,
                            f"§{sn} in IO row {sec.get('n')} Depends-On (target={target}) "
                            f"does not match any `## {sn}.` heading in {target_file}",
                        ))
    return findings


# --- Check 3: Orphaned Implementation Order row --------------------------

def check_orphaned_io_row(nodes: list) -> list:
    findings: list = []
    for n in nodes:
        rel = n["file_path"]
        row_ns = {s["n"] for s in n.get("sections", []) or [] if s.get("n") is not None}
        head_ns = {h["n"] for h in n.get("section_headings", []) or []}
        # Master-table TODOs (e.g. todo/10-platform-services/TODO-A...) use
        # named `## Tier 1` / `## Methodology` headings instead of the
        # standard `## N.` numbered form. When a file has IO rows but ZERO
        # numbered headings, the per-row check would always fail; treat that
        # as a non-standard format and skip rather than spam the operator.
        if row_ns and not head_ns:
            continue
        for orphan in row_ns - head_ns:
            findings.append(Finding(
                "orphan-io-row", rel,
                f"Implementation Order row §{orphan} has no matching `## {orphan}.` body section",
            ))
        for orphan in head_ns - row_ns:
            findings.append(Finding(
                "orphan-io-row", rel,
                f"`## {orphan}.` body section has no matching Implementation Order row",
            ))
    return findings


# --- Check 4: Dependency cycle -------------------------------------------

def check_dependency_cycle(nodes: list, id_index: dict) -> list:
    """Build a depends_on digraph from frontmatter and report any cycle.
    Vacuous pass when no node has frontmatter (today's pre-migration state).
    Uses Tarjan-style DFS with grey/black coloring."""
    findings: list = []
    if not id_index:
        return findings  # no frontmatter yet; nothing to graph
    graph: dict = {}
    for n in nodes:
        if n.get("id"):
            graph[n["id"]] = [d for d in (n.get("depends_on") or []) if d in id_index]
    GREY, BLACK = 1, 2
    color: dict = {}
    cycle_path: list = []

    def dfs(node, stack):
        if color.get(node) == BLACK:
            return False
        if color.get(node) == GREY:
            # Found a cycle. Slice stack from where node first appears.
            try:
                start = stack.index(node)
                cycle_path.extend(stack[start:] + [node])
            except ValueError:
                cycle_path.extend([node])
            return True
        color[node] = GREY
        stack.append(node)
        for nxt in graph.get(node, []):
            if dfs(nxt, stack):
                return True
        stack.pop()
        color[node] = BLACK
        return False

    for start_node in graph:
        if color.get(start_node) is None:
            if dfs(start_node, []):
                findings.append(Finding(
                    "dependency-cycle", "<graph>",
                    f"cycle in depends_on graph: {' -> '.join(cycle_path)}",
                ))
                return findings  # one cycle is enough; author breaks it
    return findings


# --- Check 5: Test-runner bat alignment ----------------------------------

def check_bat_alignment(nodes: list, snapshot: dict, repo_root: Path) -> list:
    """For every Test runner line that NAMES a `scripts/debug/<layer>/run-*.bat`
    path, verify the bat exists on disk. Lines that reference host-side
    runners (`bash scripts/test.sh`, `make test-X`, `bash scripts/build.sh`,
    "build-time check" prose, etc.) are out of scope for this check;
    bat-alignment is specifically about the Windows-side per-category bat
    files split into kernel/usermode/desktop subdirs on 2026-04-20, with
    `release` reserved for native-Windows release-pipeline tooling
    (build-manifest, write-usb, to-vhdx, to-iso PowerShell shims)."""
    findings: list = []
    for n in nodes:
        rel = n["file_path"]
        text = snapshot.get(rel, "")
        for ln in text.splitlines():
            if not TEST_RUNNER_LINE_RE.match(ln):
                continue
            for m in TEST_RUNNER_BAT_RE.finditer(ln):
                layer = m.group("layer").lower()
                bat = m.group("bat")
                if layer not in VALID_TEST_LAYERS:
                    findings.append(Finding(
                        "bat-alignment", rel,
                        f"Test runner layer '{layer}' not in {VALID_TEST_LAYERS} "
                        "(per the 2026-04-20 kernel/usermode/desktop split)",
                    ))
                    continue
                bat_path = repo_root / "scripts" / "debug" / layer / bat
                if not bat_path.exists():
                    findings.append(Finding(
                        "bat-alignment", rel,
                        f"Test runner bat does not exist on disk: scripts/debug/{layer}/{bat}",
                    ))
    return findings


# --- Check 6: Status-transition audit ------------------------------------

def check_status_transition(nodes: list) -> list:
    findings: list = []
    for n in nodes:
        st = n.get("status")
        if st in (None, "no-frontmatter"):
            continue  # nothing to audit until §5 migration
        rel = n["file_path"]
        rows = [s for s in n.get("sections", []) or [] if s.get("status") in {"x", " ", "/"}]
        if not rows:
            continue  # nothing to compare against
        all_done = all(s.get("status") == "x" for s in rows)
        any_done = any(s.get("status") == "x" for s in rows)
        any_pending = any(s.get("status") in {" ", "/"} for s in rows)
        if st == "done" and any_pending:
            n_pend = sum(1 for s in rows if s.get("status") in {" ", "/"})
            findings.append(Finding(
                "status-transition", rel,
                f"frontmatter status: done but {n_pend}/{len(rows)} Implementation Order "
                "rows are not [x] (mark rows or downgrade status)",
            ))
        if st == "draft" and all_done:
            findings.append(Finding(
                "status-transition", rel,
                f"frontmatter status: draft but all {len(rows)} Implementation Order rows "
                "are [x] (promote to status: active or status: done)",
            ))
    return findings


# --- Check 7: $schema reachability ---------------------------------------

def check_schema_reachability(nodes: list, snapshot: dict, repo_root: Path) -> list:
    """The cache build doesn't currently surface a per-node `$schema` value
    (it's a frontmatter convenience, not a node field). Re-read each file's
    frontmatter to find the line `$schema: <path>` and verify reachability.
    Skipped on files without frontmatter.

    Normalizes UTF-8 BOM + CRLF line endings the same way build.py does
    (Codex pass 8 M1: otherwise Windows-authored TODOs with BOM or CRLF
    frontmatter silently bypass this check and an escape-path $schema
    value would go unnoticed)."""
    findings: list = []
    schema_re = re.compile(r"^\$schema:\s*(.+?)\s*$")
    for n in nodes:
        if n.get("status") == "no-frontmatter":
            continue
        rel = n["file_path"]
        text = snapshot.get(rel, "")
        # Match build.py's split_frontmatter() tolerances.
        if text.startswith("\ufeff"):
            text = text[1:]
        if "\r\n" in text:
            text = text.replace("\r\n", "\n")
        if not text.startswith("---\n"):
            continue
        end = text.find("\n---\n", 4)
        if end < 0:
            continue
        for ln in text[4:end].splitlines():
            m = schema_re.match(ln)
            if not m:
                continue
            schema_ref = m.group(1).strip().strip("'\"")
            if not schema_ref:
                continue
            # Resolve relative to the TODO file's directory.
            todo_dir = (repo_root / rel).parent
            target = (todo_dir / schema_ref).resolve()
            # Codex pass 7 M1: reject paths that resolve OUTSIDE repo_root.
            # A crafted value like `../../../../etc/passwd` would otherwise
            # pass if the host file happens to exist, making validation
            # host-dependent and defeating the trust boundary.
            repo_root_resolved = repo_root.resolve()
            try:
                target.relative_to(repo_root_resolved)
            except ValueError:
                findings.append(Finding(
                    "schema-reachability", rel,
                    f"$schema points at {schema_ref} which resolves OUTSIDE the repo "
                    f"({target}); must point at a repo-local file",
                ))
                break
            if not target.exists():
                findings.append(Finding(
                    "schema-reachability", rel,
                    f"$schema points at {schema_ref} which resolves to {target} (does not exist)",
                ))
            break  # one $schema per file
    return findings


# --- --fix-line-numbers --------------------------------------------------

def fix_line_numbers(nodes: list, snapshot: dict, id_index: dict, path_index: dict,
                     repo_root: Path, write: bool, quiet: bool) -> tuple:
    """Re-resolve every `(item: "NAME" at line N)` in stamps. Returns
    `(updates_total, ambiguities, unresolvable_targets)`. Codex pass 6 M1
    contract: FAIL on non-unique item-name match (no first-match-wins);
    never invent a new item; require explicit --write to actually mutate.
    Codex pass 7 H2: ambiguity must surface as a non-zero exit so CI
    cannot silently treat a refused rewrite as success."""
    updates_total = 0
    files_modified: dict = {}
    ambiguities = 0
    unresolvable_targets = 0
    for n in nodes:
        rel = n["file_path"]
        text = snapshot.get(rel, "")
        new_lines: list = []
        modified = False
        for ln in text.splitlines():
            stamp = STAMP_LINE_RE.match(ln)
            if not stamp:
                new_lines.append(ln)
                continue
            rest = stamp.group("rest")
            updated_rest = rest
            # Build a list of (pos, target) pairs for every `-> XREF:` on
            # this line so each `(item: ...)` clause resolves against the
            # NEAREST preceding target, not always the first one. Multi-
            # XREF Accepted/Deferred lines carry up to 6 clauses in the
            # wild (e.g. TODO-12 SSDT stamps). Codex pass 8 H1 + pass 9 H1:
            # MUST also rebuild the line by SLICING against match offsets
            # rather than str.replace, because two clauses can share the
            # exact same literal text `(item: "X" at line 5)` bound to
            # different targets; str.replace would rewrite both to the
            # first resolved line.
            xref_positions: list = []
            for m in re.finditer(r"->\s*XREF:\s*(\S+)", rest):
                xref_positions.append((m.start(), m.group(1).rstrip(",")))
            if not xref_positions:
                new_lines.append(ln)
                continue
            # Accumulate (span_start, span_end, replacement_text) pairs
            # so we can rebuild updated_rest in one pass at the end.
            rewrites: list = []
            for item_match in ITEM_LINE_RE.finditer(rest):
                name = item_match.group("name")
                stored_n = int(item_match.group("n"))
                item_pos = item_match.start()
                # Pick the XREF target immediately preceding this item's
                # position. Linear scan is fine; at most ~6 clauses per line.
                xref_target = None
                for pos, tgt in xref_positions:
                    if pos <= item_pos:
                        xref_target = tgt
                    else:
                        break
                if xref_target is None:
                    continue
                target_file = resolve_xref_target(xref_target, rel, id_index, path_index)
                if target_file is None:
                    unresolvable_targets += 1
                    continue
                target_text = snapshot.get(target_file)
                if target_text is None:
                    target_text = (repo_root / target_file).read_text(encoding="utf-8")
                    snapshot[target_file] = target_text
                # Find the literal item name in the target file. Match must
                # be UNIQUE; ambiguity is a hard fail per Codex pass 6 M1.
                hits: list = []
                for ix, target_ln in enumerate(target_text.splitlines(), start=1):
                    if name in target_ln:
                        hits.append(ix)
                if len(hits) == 0:
                    continue  # never invent
                if len(hits) > 1:
                    ambiguities += 1
                    sys.stderr.write(
                        f"[validate.py] FAIL fix-line-numbers: ambiguous item_name "
                        f"{name!r} matches lines {hits} in {target_file} "
                        f"(stamp at {rel}); refusing to rewrite\n"
                    )
                    continue
                actual_n = hits[0]
                if actual_n == stored_n:
                    continue
                new_clause = f'(item: "{name}" at line {actual_n})'
                rewrites.append((item_match.start(), item_match.end(), new_clause))
                updates_total += 1
                modified = True
                if not quiet:
                    print(f"[validate.py] {'WRITE' if write else 'DRY-RUN'} "
                          f"{rel}: {name!r} {stored_n} -> {actual_n}")
            # Rebuild updated_rest by slicing at match offsets so two
            # clauses with identical literal text bound to different
            # targets each resolve to their own line (Codex pass 9 H1).
            if rewrites:
                parts: list = []
                cursor = 0
                for start, end, replacement in sorted(rewrites):
                    parts.append(rest[cursor:start])
                    parts.append(replacement)
                    cursor = end
                parts.append(rest[cursor:])
                updated_rest = "".join(parts)
            if modified and updated_rest != rest:
                # Preserve the stamp-line prefix `> **Accepted:** ` etc. by
                # slicing at the original match boundary rather than a
                # line-wide str.replace (the rest-span could theoretically
                # appear twice in the line, though not in practice).
                prefix_end = ln.rfind(rest)
                if prefix_end >= 0:
                    new_lines.append(ln[:prefix_end] + updated_rest + ln[prefix_end + len(rest):])
                else:
                    new_lines.append(ln)
            else:
                new_lines.append(ln)
        if modified and write:
            new_text = "\n".join(new_lines)
            if text.endswith("\n") and not new_text.endswith("\n"):
                new_text += "\n"
            (repo_root / rel).write_text(new_text, encoding="utf-8")
            files_modified[rel] = True
    if ambiguities > 0 and write:
        sys.stderr.write(
            f"[validate.py] {ambiguities} ambiguous item_name(s) refused; "
            "fix duplicates manually then re-run\n"
        )
    if not quiet:
        sys.stderr.write(
            f"[validate.py] fix-line-numbers: {updates_total} update(s), "
            f"{ambiguities} ambiguous, {unresolvable_targets} unresolvable target(s); "
            f"mode={'WRITE' if write else 'DRY-RUN'}\n"
        )
    return (updates_total, ambiguities, unresolvable_targets)


# --- main ----------------------------------------------------------------

# ----------------------------------------------------------------------
# Check 8: duplicate-id -- the schema enforces id format but NOT global
# uniqueness. A PR that renames alpha -> beta while another node already
# uses id=beta silently overwrites in build_id_index (dict), masking the
# collision from every downstream query. Codex pass 15 H2 (TODO-06 §6).
# ----------------------------------------------------------------------

def check_duplicate_id(nodes: list) -> list:
    findings: list = []
    by_id: dict = {}
    for n in nodes:
        nid = n.get("id")
        if not nid:
            continue
        by_id.setdefault(nid, []).append(n.get("file_path") or "(unknown-path)")
    for nid, paths in sorted(by_id.items()):
        if len(paths) <= 1:
            continue
        # Report once per duplicated id against its first path; include
        # the full collision list in the detail for triage.
        findings.append(Finding(
            "duplicate-id", paths[0],
            f"id '{nid}' is claimed by {len(paths)} files: {', '.join(paths)}",
            severity="FAIL",
        ))
    return findings


# ----------------------------------------------------------------------
# --diff mode (TODO-06 §6): compare current cache vs BASELINE.
# ----------------------------------------------------------------------

def _orphans(nodes: list, id_index: dict, path_index: dict) -> set:
    """Return the set of file_paths with zero inbound edges across all
    kinds (frontmatter + Implementation Order + Inputs XREF + stamps).
    Self-references don't count. Mirrors query.py's orphan logic at a
    file_path granularity."""
    inbound_targets: set = set()
    for n in nodes:
        src = n.get("file_path")
        for ref in (n.get("depends_on") or []) + (n.get("satisfies") or []):
            tgt = id_index.get(ref)
            if tgt and tgt != src:
                inbound_targets.add(tgt)
        sby = n.get("superseded_by")
        if sby:
            tgt = id_index.get(sby)
            if tgt and tgt != src:
                inbound_targets.add(tgt)
        for x in (n.get("inputs_xrefs") or []):
            if isinstance(x, dict):
                tgt = resolve_xref_target(
                    (x.get("target_path") or "").split("#", 1)[0].strip(),
                    src, id_index, path_index,
                )
                if tgt and tgt != src:
                    inbound_targets.add(tgt)
        for x in (n.get("stamps_xrefs") or []):
            if isinstance(x, dict):
                tgt = resolve_xref_target(
                    (x.get("target_path") or "").split("#", 1)[0].strip(),
                    src, id_index, path_index,
                )
                if tgt and tgt != src:
                    inbound_targets.add(tgt)
        for sec in (n.get("sections") or []):
            if not isinstance(sec, dict):
                continue
            for grp in sec.get("depends_on") or []:
                if not isinstance(grp, dict):
                    continue
                tgt_tok = grp.get("target")
                if not tgt_tok or tgt_tok == "self":
                    continue
                tgt = resolve_xref_target(tgt_tok, src, id_index, path_index)
                if tgt and tgt != src:
                    inbound_targets.add(tgt)
    all_paths = {n.get("file_path") for n in nodes if n.get("file_path")}
    return all_paths - inbound_targets


_STATUS_ORDER = {
    None: 0,
    "draft": 1,
    "active": 2,
    "blocked": 1,  # blocked is peer to draft, not a downgrade destination
    "done": 3,
    "superseded": 3,
}


def _status_rank(s) -> int:
    return _STATUS_ORDER.get(s, 0)


def _count_stale_xrefs(nodes: list, id_index: dict, path_index: dict) -> set:
    """Return the set of (source_file_path, xref_raw) tuples for every
    XREF that fails to resolve. Used by --diff to spot newly-introduced
    unresolvable refs without re-running the full check_stale_xref."""
    stale: set = set()
    for n in nodes:
        src = n.get("file_path") or ""
        for field in ("depends_on", "satisfies"):
            for ref in (n.get(field) or []):
                if ref not in id_index:
                    stale.add((src, f"{field}:{ref}"))
        sby = n.get("superseded_by")
        if sby and sby not in id_index:
            stale.add((src, f"superseded_by:{sby}"))
        for x in (n.get("inputs_xrefs") or []):
            if not isinstance(x, dict):
                continue
            raw = (x.get("target_path") or "").split("#", 1)[0].strip()
            if raw and resolve_xref_target(raw, src, id_index, path_index) is None:
                stale.add((src, f"inputs:{raw}"))
        for x in (n.get("stamps_xrefs") or []):
            if not isinstance(x, dict):
                continue
            raw = (x.get("target_path") or "").split("#", 1)[0].strip()
            if raw and resolve_xref_target(raw, src, id_index, path_index) is None:
                stale.add((src, f"stamps:{raw}"))
        for sec in (n.get("sections") or []):
            if not isinstance(sec, dict):
                continue
            for grp in sec.get("depends_on") or []:
                if not isinstance(grp, dict):
                    continue
                tgt = grp.get("target")
                if not tgt or tgt == "self":
                    continue
                if resolve_xref_target(tgt, src, id_index, path_index) is None:
                    stale.add((src, f"sections:{tgt}"))
    return stale


def diff_caches(baseline_path: Path, current_nodes: list, quiet: bool) -> list:
    """Return a list of Finding objects describing regressions the PR
    introduced: added orphans, new broken backlinks (ids present in
    baseline but missing in current), status downgrades (done -> active,
    etc), and newly stale XREFs. Each finding is tagged under a single
    `graph-delta` check name so it surfaces consistently in CI output."""
    findings: list = []
    try:
        baseline_nodes = json.loads(baseline_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        findings.append(Finding(
            "graph-delta", str(baseline_path),
            f"baseline cache unreadable: {exc}", severity="FAIL",
        ))
        return findings
    if not isinstance(baseline_nodes, list):
        findings.append(Finding(
            "graph-delta", str(baseline_path),
            f"baseline cache is not a list; got {type(baseline_nodes).__name__}",
            severity="FAIL",
        ))
        return findings

    base_idx = build_id_index(baseline_nodes)
    base_paths = build_path_index(baseline_nodes)
    cur_idx = build_id_index(current_nodes)
    cur_paths = build_path_index(current_nodes)

    # 1. Broken backlinks: an id present in baseline but missing from current
    # AND still referenced by some CURRENT node's depends_on / satisfies /
    # superseded_by is a regression. Codex pass 15 H1: a bare
    # `missing_ids = base_idx - cur_idx` check produced false positives
    # when a PR corrected a typo id that nobody referenced. Only flag
    # removals that an active reference still points at.
    missing_ids = set(base_idx) - set(cur_idx)
    if missing_ids:
        cur_referenced_ids: set = set()
        cur_ref_sources: dict = {}
        for n in current_nodes:
            src = n.get("file_path") or ""
            for field in ("depends_on", "satisfies"):
                for ref in (n.get(field) or []):
                    cur_referenced_ids.add(ref)
                    cur_ref_sources.setdefault(ref, []).append(f"{src}:{field}")
            sby = n.get("superseded_by")
            if sby:
                cur_referenced_ids.add(sby)
                cur_ref_sources.setdefault(sby, []).append(f"{src}:superseded_by")
        for mid in sorted(missing_ids & cur_referenced_ids):
            sources = ", ".join(cur_ref_sources.get(mid, ["(unknown)"])[:3])
            findings.append(Finding(
                "graph-delta", base_idx[mid],
                f"broken-backlink: id '{mid}' removed from current cache; "
                f"still referenced by: {sources}",
                severity="FAIL",
            ))

    # 2. Added orphans: file_paths that had inbound edges in baseline but
    # are orphaned in the current cache.
    base_orphans = _orphans(baseline_nodes, base_idx, base_paths)
    cur_orphans = _orphans(current_nodes, cur_idx, cur_paths)
    new_orphans = cur_orphans - base_orphans
    for fp in sorted(new_orphans):
        findings.append(Finding(
            "graph-delta", fp,
            "added-orphan: had inbound edges in baseline; none in current",
            severity="FAIL",
        ))

    # 3. Status downgrades: same id, rank(current) < rank(baseline).
    base_status = {n["id"]: n.get("status") for n in baseline_nodes if n.get("id")}
    cur_status = {n["id"]: n.get("status") for n in current_nodes if n.get("id")}
    for nid, cur_s in cur_status.items():
        if nid not in base_status:
            continue
        base_s = base_status[nid]
        if _status_rank(cur_s) < _status_rank(base_s):
            fp = cur_idx.get(nid, "")
            findings.append(Finding(
                "graph-delta", fp,
                f"status-downgrade: '{nid}' went from '{base_s}' to '{cur_s}'",
                severity="FAIL",
            ))

    # 4. Newly stale XREFs: XREFs that resolved in baseline but not now.
    base_stale = _count_stale_xrefs(baseline_nodes, base_idx, base_paths)
    cur_stale = _count_stale_xrefs(current_nodes, cur_idx, cur_paths)
    new_stale = cur_stale - base_stale
    for src, tok in sorted(new_stale):
        findings.append(Finding(
            "graph-delta", src,
            f"newly-stale-xref: '{tok}' resolved in baseline but not in current",
            severity="FAIL",
        ))

    return findings


def main() -> int:
    ap = argparse.ArgumentParser(
        description="TODO graph integrity validator (consumes build/todo-cache.json)"
    )
    ap.add_argument("--cache", default="build/todo-cache.json",
                    help="Path to cache file (default: build/todo-cache.json)")
    ap.add_argument("--repo-root", default=None,
                    help="Repo root (default: walk up from cwd to find scripts/todo-graph)")
    ap.add_argument("--quiet", action="store_true",
                    help="Suppress per-check PASS lines; print only failures + summary")
    ap.add_argument("--warnings-only", action="store_true",
                    help="Downgrade check-1 stale-XREF FAILs to WARNs (migration mode)")
    ap.add_argument("--fix-line-numbers", action="store_true",
                    help="Re-resolve (item: \"NAME\" at line N) parentheticals; "
                         "DRY-RUN by default. Add --write to actually rewrite files.")
    ap.add_argument("--write", action="store_true",
                    help="Required with --fix-line-numbers to actually mutate files")
    ap.add_argument("--diff", default=None, metavar="BASELINE",
                    help="Compare the current cache against BASELINE and report "
                         "regressions the PR introduced: added orphans, broken "
                         "backlinks, status downgrades, newly stale XREFs. "
                         "Exit 1 if any regression appears, 0 otherwise.")
    args = ap.parse_args()

    if args.write and not args.fix_line_numbers:
        sys.stderr.write("[validate.py] FATAL: --write requires --fix-line-numbers\n")
        return 2

    # Resolve repo root. When --repo-root is explicit (regression tests using
    # synthetic fixture trees without a scripts/todo-graph dir), trust it.
    # When auto-discovering, walk up from cwd until we find the real repo.
    if args.repo_root:
        repo_root = Path(args.repo_root).resolve()
    else:
        cwd = Path.cwd().resolve()
        repo_root = cwd
        for _ in range(8):
            if (repo_root / "scripts/todo-graph/build.py").exists():
                break
            if repo_root.parent == repo_root:
                break
            repo_root = repo_root.parent
        if not (repo_root / "scripts/todo-graph/build.py").exists():
            sys.stderr.write(f"[validate.py] FATAL: cannot find scripts/todo-graph/build.py from {repo_root}\n")
            return 2

    cache_path = Path(args.cache)
    if not cache_path.is_absolute():
        cache_path = repo_root / cache_path
    nodes = load_or_rebuild_cache(cache_path, repo_root, args.quiet)

    snapshot = load_file_snapshot(repo_root, nodes)
    id_index = build_id_index(nodes)
    path_index = build_path_index(nodes)

    if args.fix_line_numbers:
        _updates, ambig, _unres = fix_line_numbers(
            nodes, snapshot, id_index, path_index,
            repo_root, write=args.write, quiet=args.quiet,
        )
        # Codex pass 7 H2: ambiguity is a hard failure. --write that refused
        # to rewrite because of non-unique item-name match must exit non-zero
        # so CI or hooks cannot silently treat a refused rewrite as success.
        if ambig > 0:
            return 1
        return 0

    # Run all 7 checks.
    all_findings: list = []
    check_results: list = []
    for name, check_fn, args_tuple in [
        ("stale-xref", check_stale_xref, (nodes, id_index, path_index, args.warnings_only)),
        ("dangling-section", check_dangling_section_ref, (nodes, id_index, path_index)),
        ("orphan-io-row", check_orphaned_io_row, (nodes,)),
        ("dependency-cycle", check_dependency_cycle, (nodes, id_index)),
        ("bat-alignment", check_bat_alignment, (nodes, snapshot, repo_root)),
        ("status-transition", check_status_transition, (nodes,)),
        ("schema-reachability", check_schema_reachability, (nodes, snapshot, repo_root)),
        ("duplicate-id", check_duplicate_id, (nodes,)),
    ]:
        findings = check_fn(*args_tuple)
        check_results.append((name, findings))
        all_findings.extend(findings)

    # Print findings + per-check pass/fail summary.
    fail_count = 0
    warn_count = 0
    for name, findings in check_results:
        fails = [f for f in findings if f.severity == "FAIL"]
        warns = [f for f in findings if f.severity == "WARN"]
        if not fails and not warns:
            if not args.quiet:
                print(f"  [PASS] {name}")
        else:
            if fails:
                fail_count += len(fails)
                print(f"  [FAIL] {name}: {len(fails)} failure(s)")
                for f in fails:
                    print(f"    {f.format()}")
            if warns:
                warn_count += len(warns)
                if not args.quiet:
                    print(f"  [WARN] {name}: {len(warns)} warning(s)")
                    for f in warns:
                        print(f"    {f.format()}")

    # --diff: run graph-delta check against BASELINE. A delta finding
    # counts as a failure even if all 7 primary checks pass; this is
    # the "this PR makes the graph worse" signal for code review.
    delta_fail = 0
    if args.diff:
        baseline_path = Path(args.diff)
        if not baseline_path.is_absolute():
            baseline_path = (repo_root / baseline_path).resolve()
        deltas = diff_caches(baseline_path, nodes, args.quiet)
        delta_fails = [f for f in deltas if f.severity == "FAIL"]
        if not delta_fails:
            if not args.quiet:
                print("  [PASS] graph-delta")
        else:
            delta_fail = len(delta_fails)
            print(f"  [FAIL] graph-delta: {delta_fail} regression(s) vs {baseline_path}")
            for f in delta_fails:
                print(f"    {f.format()}")
        fail_count += delta_fail

    # Trailing summary
    total_pass = sum(1 for _, f in check_results if not [x for x in f if x.severity == "FAIL"])
    total = len(check_results)
    if args.diff:
        print(f"[validate.py] {total_pass}/{total} checks passed, {fail_count} failure(s) "
              f"({delta_fail} graph-delta vs baseline), {warn_count} warning(s)")
    else:
        print(f"[validate.py] {total_pass}/{total} checks passed, {fail_count} failure(s), {warn_count} warning(s)")
    return 0 if fail_count == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
