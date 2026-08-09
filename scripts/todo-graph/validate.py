#!/usr/bin/env python3
# ============================================================================
# scripts/todo-graph/validate.py -- TODO graph integrity validator
#
# Owner: TODO-06 §3 (Validator) in todo/00-infrastructure/.
# Consumer of: build/todo-cache.json (produced by scripts/todo-graph/build.py).
#
# Runs eight graph-integrity checks on the cache and the live TODO files.
# Each check is independent; failures are tagged by category and printed in
# the t_pass / t_fail style used by scripts/test-tooling.sh.
#
# EXIT CODES (rebuild-recovery and baseline-cache work in the TODO metadata
# layer). rc 2 was reachable long before it was written down -- build.py-not-
# found and the argument errors below all took it -- while this header
# documented only 0/1, so an infrastructure failure that escaped as a traceback
# landed on rc 1 and read as a graph verdict. The three codes are now a
# contract, and scripts/todo-graph/build-and-validate.sh repeats it:
#
#   0  clean: every check passed (and, under --diff, no graph delta).
#   1  GRAPH FINDINGS: the corpus has problems a human should fix. This code
#      means the validator RAN and reached a verdict.
#   2  usage error or INFRASTRUCTURE REFUSAL: bad arguments, build.py missing,
#      or the cache/baseline could not be trusted to answer with. This code
#      means there is NO verdict. Nothing about the corpus is asserted, and no
#      findings are printed -- see the output-buffering note in main().
#
# The distinction is load-bearing in one direction: a refusal misreported as
# rc 1 sends someone to fix a graph that was never examined, and a false
# finding is worse than a refusal because it is actioned.
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
#      the todo tree. Added 2026-04-23 to catch rename-into-collision
#      PRs that build_id_index would otherwise mask.
#   9. Duplicate resolver key: the DERIVED (domain, TODO-number) identity
#      must be unique too. Check 8 covered only the authored `id`, so a
#      collision on the derived key -- which every compact reference and
#      every query reader binds through -- passed here while the shared
#      cache validator refused the same cache outright.
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
import os
import re
import stat
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath
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
# CAPTURES THE WHOLE DIRECTORY, not just its two-digit prefix: the resolver
# looks a directory-qualified reference up by the directory the author wrote
# (section 26), so throwing `-kernel-core` away here is what let `14-alpha/...`
# be answered by `14-beta`.
DOMAIN_PATH_RE = re.compile(r"^(?P<dir>\d{2}-[a-z0-9-]+)/TODO-(?P<num>\d{1,2})$")
# A numbered domain directory: `NN-slug`. Mirrors the shape `cache_schema.
# resolver_key` matches on `parts[-2]`, but is applied to the DIRECTORY alone so
# a folder holding only letter-numbered TODOs still counts as indexed.
_NUMBERED_DIR_RE = re.compile(r"^\d{2}-")
# `[label](destination)`. The label may itself contain brackets (`[`TODO-06
# §22`]`), so it is matched greedily up to the LAST `](` rather than with a
# `[^\]]*` class that stops at the first inner bracket. The destination excludes
# whitespace and parentheses, which also rules out a link title (`(url "t")`) --
# a shape this corpus does not use and which must not be silently truncated to
# the url and resolved.
#
# EXACTLY ONE LINK, and the label may not swallow another one. A greedy `.*`
# label treats `[a](x.md)[b](y.md)` -- which the `(\S+)` XREF parser hands over
# whole -- as one link whose destination is `y.md`, so a malformed multi-target
# token resolved to its LAST destination instead of being refused (Codex
# adversarial, section 26 review).
#
# THE BAN ON `](` IS EXPRESSED AS A LOOKAHEAD, not as a character class. The
# first cut allowed "an ordinary char OR a bracket pair", and the two
# alternatives together still admitted `](`: the pair alternative ends at `]`
# and the ordinary alternative then accepts `(`. So
# `[[a](x.md)[b](y.md)](z.md)` matched and resolved `z.md` -- the same silent
# multi-destination bind, one nesting level down (Codex re-adversarial, section
# 26 review round 2). Refusing the SEQUENCE at every position is what actually
# states the rule; a label carrying ordinary brackets still matches.
_MD_LINK_RE = re.compile(
    r"^\[(?P<label>(?:(?!\]\().)*)\]\((?P<url>[^()\s]*)\)$")
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

# Reasons a REBUILD can plausibly repair, because the cache is a DERIVED
# artifact and regenerating it is the documented recovery. Everything outside
# this set refuses instead.
#
# STALE IS DELIBERATELY NOT IN IT, and that is the one entry worth arguing
# about. Staleness is exactly what a rebuild fixes, so including it looks
# obviously right -- but `--cache` names an arbitrary path and the recovery
# writes the rebuilt artifact BACK to that path (`--output str(cache_path)`
# below), so auto-rebuilding on staleness would DESTROY any captured or
# imported cache whose corpus binding merely differs from this checkout. That
# is data loss triggered by the ordinary, expected state of a historical
# artifact (Codex design review, section 23, [high]). A stale cache refuses
# with rc 2 and the operator re-runs build-and-validate.sh, which is the
# explicit, non-destructive form of the same repair.
#
# LEGACY_NO_STAMPED_ITEMS is absent for a different reason: PROFILE_VALIDATE
# does not set `require_stamped_population`, so this reader can never raise it.
#
# LEGACY_FORMAT joins the set for the reason the set exists: a cache whose
# corpus binding declares a producer contract this reader does not speak (or
# declares none at all) is repaired by running the current producer -- exactly what a
# MISSING or SHAPE cache is repaired by. It is deliberately NOT grouped with
# SHAPE, because the two need different words to the operator (that artifact was
# valid under the contract it was written against) and because only the
# ownership gate below decides whether the repair may WRITE: a historical
# `--diff` baseline in this state is refused with its bytes intact.
_REBUILDABLE_REASONS = frozenset((
    cache_schema.REASON_MISSING,
    cache_schema.REASON_UNREADABLE,
    cache_schema.REASON_SHAPE,
    cache_schema.REASON_EMPTY,
    cache_schema.REASON_LEGACY_FORMAT,
))

# The ONE path this tool owns and may therefore destroy. It is the argparse
# default for --cache, which is the point: a caller who did not name a cache is
# asking for the generated one, and regenerating a file we generate is not data
# loss.
#
# EXCLUDING STALE ALONE WAS NOT ENOUGH, and stopping there was the comfortable
# half-measure. The rebuild writes `--output <that same path>`, so UNREADABLE /
# SHAPE / EMPTY destroy a caller-named artifact just as thoroughly -- and SHAPE
# is the likely one, because an imported or captured older cache is exactly
# what fails a newly-added subtree. OWNERSHIP, not reason, is what decides
# whether overwriting is allowed (Codex adversarial, section 23, [high]).
_CANONICAL_CACHE_REL = "build/todo-cache.json"


def _is_canonical_cache(cache_path: Path, repo_root: Path) -> bool:
    """Is this the generated cache -- the one file the recovery may overwrite?

    Two failure modes a `==` on Path objects has, and both matter because the
    answer authorises a destructive write:

    - Equivalent spellings compare UNEQUAL. `--cache build/../build/todo-cache.json`
      and `--cache ./build/todo-cache.json` both name the generated cache, and
      both were treated as somebody else's file, so the recovery refused a cache
      it does own.
    - Equal spellings can name a DIFFERENT file. If `build/` is a symlink into a
      shared or external directory, the canonical spelling still compares equal
      and authorises overwriting a cache outside this repository entirely.

    So compare resolved paths, and additionally require the resolved cache to
    remain under the resolved repo root -- a symlinked `build` then fails the
    containment test rather than smuggling the target out (Codex adversarial,
    section 23 review, [medium]).
    """
    try:
        root = Path(os.path.realpath(repo_root))
        resolved = Path(os.path.realpath(cache_path))
        canonical = Path(os.path.realpath(repo_root / _CANONICAL_CACHE_REL))
    except OSError:
        return False
    if resolved != canonical:
        return False
    # RESOLVING BOTH SIDES CUTS THE OTHER WAY TOO, which is what the previous
    # revision missed. If `build/todo-cache.json` is itself a SYMLINK to a
    # tracked TODO, both realpaths equal that victim and the containment test
    # passes -- so a corrupt cache is classified as owned, and the recovery
    # writes cache JSON over the file the link points at. The ownership gate
    # would have authorised the exact data loss it was added to prevent (Codex
    # re-adversarial round 7, section 23 review, [high]).
    #
    # Ownership therefore requires the path to contain NO symlink at all: a
    # purely lexical normalisation must agree with the resolved one. That still
    # accepts every equivalent spelling (`build/../build/todo-cache.json`),
    # because normpath collapses those without touching the filesystem.
    lexical = Path(os.path.abspath(os.path.normpath(cache_path)))
    if lexical != resolved:
        return False
    try:
        resolved.relative_to(root)
    except ValueError:
        return False
    return True


def _refuse(message: str) -> None:
    """Every infrastructure refusal leaves through here, at rc 2, so it can
    never be confused with the rc 1 the nine checks return."""
    sys.stderr.write(f"[validate.py] REFUSED: {message}\n")
    sys.exit(2)


def _reverify_corpus(todo_root: Path, cache_info) -> None:
    """Close the generation binding the load only OPENED.

    `check_freshness` proves the cache matched the corpus at ONE instant, at
    load time. This validator then spends its whole run walking the live TODO
    files -- nine checks plus, under --diff, a delta pass -- and that walk is
    precisely the long window the binding exists to cover. Without this call the
    guarantee was a few milliseconds wide while the exposure was the entire run.
    """
    try:
        cache_schema.check_corpus_unchanged(todo_root, cache_info.corpus)
    except cache_schema.CacheSchemaError as exc:
        _refuse(f"the TODO corpus changed while the checks were running, so "
                f"the findings describe a tree that no longer exists "
                f"[{exc.reason}]: {exc}")


def load_or_rebuild_cache(cache_path: Path, repo_root: Path, quiet: bool):
    """Load and VALIDATE the cache, rebuilding it AT MOST ONCE. Returns
    `(nodes, CacheInfo)`.

    Rebuilding a derived artifact is sound recovery and is kept. TRUSTING the
    result was the defect: the previous implementation ended in a bare
    `json.loads(cache_path.read_text())` of whatever build.py had just written,
    so a corrupt post-rebuild cache exited 1 with a traceback -- the code this
    validator documents as GRAPH FINDINGS. It also read and parsed the file
    TWICE (once to probe the shape, once to return it), both times unbounded and
    with the first node graph still live during the second parse.

    Both close together by routing through `cache_schema.load_and_validate`,
    which reads once through a bounded, generation-bound descriptor and applies
    the profile this reader declares. The recovery is then: validate; if the
    reason is one a rebuild can repair, rebuild EXACTLY ONCE and validate again;
    a second failure is rc 2. There is no loop -- retrying against a
    deterministically-corrupt producer is a hang, not a recovery.
    """
    todo_root = repo_root / "todo"
    try:
        return cache_schema.load_and_validate(
            cache_path, todo_root, profile=cache_schema.PROFILE_VALIDATE)
    except cache_schema.CacheSchemaError as first:
        # PHYSICAL, not lexical. `--cache build/../build/todo-cache.json`
        # names the file this tool owns and compared unequal; conversely the
        # canonical spelling compared EQUAL even when `build` is a symlink into
        # a shared directory, which would authorise overwriting a cache outside
        # the repository. Resolve both, and require the result to still sit
        # under the resolved repo root so a symlinked `build` cannot smuggle
        # the target out (Codex adversarial, section 23 review, [medium]).
        owned = _is_canonical_cache(cache_path, repo_root)
        if first.reason in _REBUILDABLE_REASONS and not owned:
            _refuse(f"cache at {cache_path} is unusable [{first.reason}]: "
                    f"{first}. Not rebuilt automatically: this is not the "
                    f"generated cache ({repo_root / _CANONICAL_CACHE_REL}), so "
                    f"rebuilding would overwrite a file this tool does not own. "
                    f"Run build.py --output <that path> yourself if it is "
                    f"disposable, or point --cache at the generated cache")
        if first.reason not in _REBUILDABLE_REASONS:
            # The membership test comes FIRST so the set stays the single
            # switch: a reason added to `_REBUILDABLE_REASONS` becomes
            # rebuildable, full stop, with no special case silently overriding
            # it further up. Only the WORDING branches below.
            if first.reason == cache_schema.REASON_STALE:
                # Worded separately because the honest answer differs: a
                # rebuild WOULD repair this one, and declining is a deliberate
                # choice not to overwrite a file the caller named. "A rebuild
                # would not repair it" would be false here and would send the
                # operator looking for a fault that is not there.
                # Not rebuilt even when the cache IS the one this tool owns.
                # Ownership decides whether overwriting is PERMITTED; this
                # refusal is about something else -- silently regenerating a
                # stale cache would hide the fact that someone forgot to
                # rebuild, and hiding it is how a run certifies a corpus
                # nobody re-derived. `build-and-validate.sh` is the sanctioned
                # rebuild-then-check path and it is one command.
                _refuse(f"cache at {cache_path} does not match the corpus "
                        f"[{first.reason}]: {first}. Not rebuilt "
                        f"automatically -- regenerating it here would hide "
                        f"the missed rebuild rather than report it. Run "
                        f"scripts/todo-graph/build-and-validate.sh, which "
                        f"rebuilds and then checks")
            _refuse(f"cache at {cache_path} cannot be used and a rebuild would "
                    f"not repair it [{first.reason}]: {first}")
        if not quiet:
            sys.stderr.write(
                f"[validate.py] cache at {cache_path} unusable "
                f"[{first.reason}]: {first}; rebuilding via build.py (once)\n")

    # Locate build.py via validate.py's own directory (they're siblings);
    # don't assume repo_root/scripts/todo-graph/ since --repo-root for
    # regression fixtures points at a synthetic tree.
    build_py = Path(__file__).resolve().parent / "build.py"
    if not build_py.exists():
        _refuse(f"build.py not found next to validate.py ({build_py})")
    result = subprocess.run(
        [sys.executable, str(build_py),
         "--quiet", "--output", str(cache_path),
         "--root", str(todo_root),
         "--repo-root", str(repo_root)],
        cwd=str(repo_root),
    )
    if result.returncode != 0:
        _refuse(f"build.py failed while rebuilding {cache_path} "
                f"(exit {result.returncode})")

    try:
        return cache_schema.load_and_validate(
            cache_path, todo_root, profile=cache_schema.PROFILE_VALIDATE)
    except cache_schema.CacheSchemaError as second:
        # The rebuild ran and its output is still unusable, so the producer --
        # not the artifact -- is what is broken. One rebuild, one re-validation,
        # then stop.
        _refuse(f"cache at {cache_path} is still unusable after one rebuild "
                f"[{second.reason}]: {second}")


# `_cache_shape_is_stale` lived here and is GONE, not moved. It hand-detected
# ONE shape defect -- a `sections[].depends_on` group that was still an opaque
# string rather than a `{target, sections[]}` dict -- so the loader could force a
# rebuild instead of crashing later on `grp.get(...)`. Routing the loader through
# `cache_schema` subsumes it strictly: `_validate_section_deps` refuses that same
# group with REASON_SHAPE, which is in `_REBUILDABLE_REASONS`, so the recovery
# still fires -- and every OTHER shape defect it never looked for now fires too.
# Keeping a second, narrower detector beside the shared validator is how the two
# drift apart, which is the failure the shared validator exists to end.


def load_file_snapshot(repo_root: Path, nodes: list) -> dict:
    """Read every TODO file's text ONCE at startup and return a mapping
    file_path -> text. Single in-memory snapshot per run so all checks (and
    --fix-line-numbers, if enabled) observe identical repo state. Codex
    pass 6 M1 atomic-snapshot contract.

    FAIL CLOSED, and BOUNDED. Both were fail-open before: a read error became
    `snapshot[rel] = ""` behind a WARN, and the read itself was an unbounded
    `read_text`. An empty string is not a neutral value here -- it is a TODO
    file that reads as having no Implementation Order rows, no `## N.` bodies
    and no test-runner lines, so a TRANSIENT read failure (ENFILE, a file being
    rewritten under us, a permission blip) manufactures orphan-IO-row and
    dangling-section FAILs at rc 1, against a file nobody has touched. That is
    the infrastructure-failure-reported-as-a-verdict collision this validator's
    exit-code contract exists to prevent, so an unreadable TODO is now a
    refusal (Codex design review, section 23, [high]).

    The read goes through `cache_schema.read_corpus_file`, which is the same
    bounded reader the freshness fingerprint uses -- one rule, one
    implementation, and a 16 MiB per-file ceiling enforced BEFORE the bytes are
    materialised.
    """
    snapshot = {}
    for n in nodes:
        rel = n.get("file_path")
        if not rel:
            continue
        full = repo_root / rel
        try:
            snapshot[rel] = cache_schema.read_corpus_file(full).decode("utf-8")
        except cache_schema.CacheSchemaError as exc:
            _refuse(f"cannot read TODO file {rel}, so no verdict can be "
                    f"reached about it [{exc.reason}]: {exc}")
        except (OSError, ValueError) as exc:
            _refuse(f"cannot read TODO file {rel}, so no verdict can be "
                    f"reached about it: {exc}")
    return snapshot


# --- XREF resolver -------------------------------------------------------

def build_id_index(nodes: list) -> dict:
    """Return {id: file_path} for every node carrying a non-null id. Used
    by check 1 (stale-XREF) to look up frontmatter id references."""
    return {n["id"]: n["file_path"] for n in nodes if n.get("id")}


def build_path_index(nodes: list) -> dict:
    """Return {(domain, todo_number): file_path} for every node, plus
    {filename_stem: file_path} for unambiguous filename references.
    Used by the resolver below for compact-form XREFs.

    A DOMAIN CODE IS NOT A DIRECTORY, and treating it as one was the whole of
    section 26. The two-digit code is a PREFIX of the directory name, so
    `todo/14-alpha/` and `todo/14-beta/` share the code `14` -- and every
    resolver branch reduced a reference to `(code, number)` and looked it up in
    `by_dn`, discarding the directory the author actually wrote. That is a
    WRONG BINDING rather than a coin flip wherever the reference names its
    directory: `14-alpha/TODO-02` resolved to `todo/14-beta/TODO-02-b.md` in
    both node orders, because only the code survived to the lookup.

    So the index carries the directory as a first-class key:

      by_dirnum  {(dir_name, number): file_path}  -- what a DIRECTORY-QUALIFIED
                 or same-directory reference resolves through. Unambiguous by
                 construction on a valid cache (two files sharing it would also
                 share a `(code, number)` key, which `cache_schema` refuses),
                 but its own collision set is recorded anyway rather than
                 assumed -- an assumption is how the flattened maps above came
                 to return the survivor of an overwrite.
      by_code    {code: frozenset(dir_name)} -- how many directories a code
                 owns, which is what makes the compact `D14` form decidable:
                 exactly one directory resolves, anything else refuses.
      dirs       every indexed directory name, so an explicitly named one can
                 be checked for EXISTENCE instead of being inferred from a code
                 that some other directory happens to share.

    `by_dn` keeps its shape and its collision rule: a reference that names only
    a code (`D02T19`) genuinely asks a code-level question and is still
    answered -- and still refused when two files share the pair."""
    by_dn = {}
    by_filename = {}
    dn_collisions = set()
    stem_collisions = set()
    stem_paths = {}
    by_dirnum = {}
    dirnum_paths: dict = {}
    dirnum_collisions = set()
    by_code: dict = {}
    dir_paths: dict = {}
    dir_path_collisions = set()
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
            dkey = (_path_dir(rel), key[1])
            if dkey in by_dirnum:
                dirnum_collisions.add(dkey)
            by_dirnum[dkey] = rel
            # EVERY candidate for the slot, not just the survivor -- `by_dirnum`
            # keeps the last one, so the precomputed colliding-path set built
            # from it alone would silently omit the file that lost the
            # overwrite, and that file is exactly the one a reference naming it
            # must be refused for.
            dirnum_paths.setdefault(dkey, []).append(rel)
        # DIRECTORY MEMBERSHIP IS INDEPENDENT OF THE NUMERIC KEY, and this
        # separation is load-bearing (Codex adversarial, section 26, [medium]).
        # `resolver_key` answers "which (code, number) slot" and so requires a
        # NUMERIC `TODO-NN-` filename -- but a directory whose only file is a
        # LETTER-numbered one (`TODO-A-...`, a supported catalog shape) is a
        # real, indexed directory all the same. Deriving membership inside the
        # numeric branch made such a directory invisible: `D14` then saw the
        # code as owning ONE directory and resolved confidently to the wrong
        # one, and `14-beta` was refused although the node was indexed -- a
        # wrong answer and an over-refusal from the same omission.
        dir_name = _path_dir(rel)
        if dir_name and _NUMBERED_DIR_RE.match(dir_name):
            by_code.setdefault(dir_name[:2], set()).add(dir_name)
            # THE PARENT PATH, KEPT SEPARATELY FROM THE NAME. References spell
            # the bare directory (`14-alpha`) while the cache stores the full
            # path (`todo/14-alpha/TODO-01-a.md`), and the INDEX.md answer must
            # be the full one -- so the map from one to the other has to be
            # recorded rather than reconstructed by prepending a hardcoded
            # `todo/`. Two parents sharing a basename would make that map
            # ambiguous, which is recorded and refused for the same reason
            # every other ambiguity here is: returning either one is a wrong
            # answer that looks right.
            parent = str(PurePosixPath(rel).parent)
            if dir_paths.setdefault(dir_name, parent) != parent:
                dir_path_collisions.add(dir_name)
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
    # PRECOMPUTED ONCE, and it is a SNAPSHOT. `_all_paths` rebuilt this set on
    # every relative/path-like reference -- `resolve_xref_target` calls it once
    # per graph edge that takes those branches -- turning an O(E) walk into
    # O(E*V). Only ~5-6ms on today's corpus (78 of 2,895 resolution calls reach
    # it), but a synthetic path-heavy corpus went 45.9ms at 500 nodes to 1.815s
    # at 4,000: 39.5x for 8x the input. A scaling cliff, not a present cost.
    #
    # `frozenset`, and stated plainly because it is the honest limit: this is a
    # snapshot of the maps as they stand HERE. Mutating `by_filename` or
    # `stem_paths` afterwards does NOT refresh it, so a caller that wants to add
    # a node rebuilds the index rather than patching a component map.
    # Both unions are written with a zero-argument-safe base: `s.union()` with
    # no operands returns a copy, so an empty corpus needs no special case.
    all_paths = frozenset(set(by_filename.values()).union(*stem_paths.values()))
    # SORTED ONCE, not per edge. The letter branch built a set union over every
    # stem candidate and SORTED it on each reference it handled -- O(V log V)
    # per edge against a corpus that only grows (Codex perf, section 26 review,
    # [high]). The order is part of the contract (it makes an ambiguous letter
    # match deterministic), so it is preserved and merely hoisted.
    sorted_paths = tuple(sorted(
        {q for ps in stem_paths.values() for q in ps} or all_paths))
    # AND THE COLLISION ANSWER IS PRECOMPUTED TOO. `_collides` re-derived a
    # path's `(directory, number)` key on every exact-stem or exact-path answer,
    # re-running `resolver_key` and `_path_dir` over facts this loop already
    # established: 1,177 calls across 2,673 targets on the live 232-node cache,
    # about a third of measured resolution time (20.29ms -> 13.58ms median once
    # indexed; 187.4ms -> 146.5ms at 4,000 relative edges).
    colliding_paths = frozenset(
        rel for dkey in dirnum_collisions for rel in dirnum_paths.get(dkey, ()))
    return {"by_dn": by_dn, "by_filename": by_filename,
            "dn_collisions": dn_collisions, "stem_collisions": stem_collisions,
            "stem_paths": stem_paths,
            "by_dirnum": by_dirnum, "dirnum_collisions": dirnum_collisions,
            "by_code": {c: frozenset(d) for c, d in by_code.items()},
            "dir_paths": dir_paths, "dir_path_collisions": dir_path_collisions,
            "all_paths": all_paths, "sorted_paths": sorted_paths,
            "colliding_paths": colliding_paths}


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


def _path_dir(file_path: str) -> Optional[str]:
    """The directory component of a cache `file_path`, or None.

    ONE derivation for "which directory is this node in", used by the index and
    by every branch that compares a reference's directory to a candidate's.
    The hand-rolled `fp.split("/")[1][:2]` it replaces did two things at once --
    assumed a `todo/` prefix and truncated to the CODE -- which is exactly how
    the letter branch kept binding across directories after every other branch
    had been corrected."""
    parts = PurePosixPath(file_path).parts
    return parts[-2] if len(parts) >= 2 else None


def _collides(path_index: dict, file_path: Optional[str]) -> bool:
    """Does this file sit in a `(DIRECTORY, number)` slot two files share?

    DIRECTORY-LEVEL, NOT CODE-LEVEL, and that distinction is this whole
    section's thesis applied to its own guard. Asking `dn_collisions` -- which
    is keyed on the two-digit CODE -- refused `TODO-07-shared-name.md` when
    `14-alpha` and `14-beta` each held a number 07, although the two files are
    in different directories and `by_dirnum` tells them apart perfectly. That is
    the code-standing-in-for-a-directory error one more time, in the guard added
    to fix it (caught by this section's own duplicate-stem fixture).

    SPELLING MUST NOT DECIDE SAFETY. Section 22 refuses a reference into a
    colliding slot because whichever file the index kept would silently rebind
    the other -- but that refusal was reachable only through the branches that
    consult `by_dn`. The exact-stem and exact-path branches answered the SAME
    target spelled differently: with `19-x/TODO-01-first.md` refused,
    `TODO-01-first.md` and `./TODO-01-first.md` both resolved (Codex
    re-adversarial, section 26 round 6, [medium]). Applying the rule to the
    ANSWER rather than to the branch makes every spelling of one target agree.
    `cache_schema` refuses such a cache outright, so this can only ever fire on
    a graph that is already invalid -- which is exactly when a confident answer
    is most misleading."""
    if not file_path:
        return False
    known = path_index.get("colliding_paths")
    if known is not None:
        return file_path in known
    key = cache_schema.resolver_key(file_path)
    if key is None:
        return False
    return (_path_dir(file_path), key[1]) in path_index.get("dirnum_collisions", ())


def _dir_paths(path_index: dict) -> dict:
    """{directory name: its parent path}, for every indexed directory.

    Derived from `by_dn`'s values when the key is absent, so an index built by
    hand (the test suite builds `{"by_filename": {}, "by_dn": {}}`) still gets
    a correct answer rather than an empty one -- an empty map here would refuse
    every named-directory reference, which is a silent over-refusal and the
    mirror image of the defect this section removes."""
    known = path_index.get("dir_paths")
    if known is not None:
        return known
    out = {}
    for fp in path_index.get("by_dn", {}).values():
        parts = PurePosixPath(fp).parts
        if len(parts) >= 2:
            out.setdefault(parts[-2], str(PurePosixPath(fp).parent))
    return out


def _index_dirs(path_index: dict) -> frozenset:
    """Every directory name the index knows."""
    return frozenset(_dir_paths(path_index))


def _dir_index_path(path_index: dict, dir_name: str) -> Optional[str]:
    """The full `<parent>/INDEX.md` path for a named directory, or None.

    None means the directory is not indexed at all, or -- pathologically -- two
    parents share its basename, in which case naming one of them would be the
    same wrong-answer-that-looks-right this section exists to remove."""
    if dir_name in path_index.get("dir_path_collisions", ()):
        return None
    parent = _dir_paths(path_index).get(dir_name)
    if parent is None:
        return None
    return f"{parent}/INDEX.md"


def _code_dirs(path_index: dict, code: str) -> frozenset:
    """The directories a two-digit domain code owns."""
    by_code = path_index.get("by_code")
    if by_code is not None:
        return by_code.get(code, frozenset())
    return frozenset(d for d in _index_dirs(path_index) if d[:2] == code)


def _code_index_path(path_index: dict, code: str) -> Optional[str]:
    """`<parent>/INDEX.md` for a code that owns EXACTLY ONE directory.

    The compact `D14` form names a code and nothing else, so when the code owns
    two directories there is no answer to give -- the old scan returned the
    first `by_dn` entry that matched, which is whichever node the cache
    happened to list first. Refusing makes it an ordinary unresolved XREF the
    author clears by naming the directory, which is the form that carries the
    missing information."""
    dirs = _code_dirs(path_index, code)
    if len(dirs) != 1:
        return None
    return _dir_index_path(path_index, next(iter(dirs)))


def _dirnum_lookup(path_index: dict, dir_name: str, num: int) -> Optional[str]:
    """Resolve a `(directory, number)` pair, refusing an ambiguous one.

    THIS IS THE LOOKUP A DIRECTORY-QUALIFIED REFERENCE DESERVES. `_dn_lookup`
    answers a question the author did not ask -- it drops the directory and
    matches on the code -- so `14-alpha/TODO-02` was answered by a file in
    `14-beta`.

    THE COMPATIBILITY PATH CHECKS THE ANSWER IT GETS BACK, and that is not
    belt-and-braces: without it this function REINTRODUCED the very defect one
    line below the docstring describing it. For an index built by hand (the
    test suite builds `{"by_filename": {}, "by_dn": {...}}`) the fallback asked
    `by_dn` for `(dir_name[:2], num)` -- the code-for-directory substitution
    again -- so `14-alpha/TODO-02` resolved to a `14-beta` file exactly as
    before, on the one path with no fixture pressure (Codex re-adversarial,
    section 26, [medium]). Verifying the candidate's own directory makes the
    fallback agree with the primary lookup instead of quietly disagreeing."""
    by_dirnum = path_index.get("by_dirnum")
    if by_dirnum is None:
        cand = _dn_lookup(path_index, (dir_name[:2], num))
        return cand if cand and _path_dir(cand) == dir_name else None
    if (dir_name, num) in path_index.get("dirnum_collisions", ()):
        return None
    return by_dirnum.get((dir_name, num))


def _src_dir(source_file: str) -> Optional[str]:
    """The directory a reference's SOURCE file lives in.

    Same-domain forms (`T17`, bare `TODO-19`) mean "the neighbour of this
    file", and the neighbour is decided by the DIRECTORY, not by the two digits
    in front of it. Reading only `[:2]` made a reference from `14-alpha`
    resolve into `14-beta`."""
    parts = PurePosixPath(source_file).parts
    return parts[-2] if len(parts) >= 2 else None


def _all_paths(path_index: dict) -> set:
    """Every file_path the index knows, from ALL stem candidates.

    PRECOMPUTED BY `build_path_index` since section 26; this is the accessor,
    and it recomputes ONLY for an index built without the key. Callers must not
    read `path_index["all_paths"]` directly -- going through here is what keeps
    a hand-built index working.

    `by_filename.values()` holds only the LAST path per stem, so an exact
    path-membership test against it is decided by cache row order: with
    duplicate supported stems in two domains, `../01-a/TODO-A-Shared.md`
    returned None while `../02-b/TODO-A-Shared.md` resolved, purely because the
    latter was inserted second. That is a FALSE stale-XREF finding on a
    perfectly good reference (Codex adversarial, section 22 round 6, [medium]).
    """
    cached = path_index.get("all_paths")
    if cached is not None:
        return cached
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
    # A MARKDOWN LINK IS A SURFACE FORM, so unwrap `[text](url)` to its url --
    # and do it BEFORE the fragment strip, which is the ordering the first cut
    # got wrong. Callers hand this function whatever their own extraction
    # produced, and `fix_line_numbers` captures an XREF token with `(\S+)`, so a
    # whole link arrives here. The old resolver answered one only by accident:
    # the embedded `/` split it into path components, `rel_parts[1]` happened to
    # be the real directory, and a loose number regex tolerated the trailing
    # `)`. Stripping the fragment first turned `[T](x.md#sec)` into the
    # unmatchable `[T](x.md` and refused every ANCHORED link -- the commonest
    # shape in this corpus (Codex re-adversarial, section 26 round 6, [medium]).
    link_m = _MD_LINK_RE.match(target)
    # A BRACKET-PREFIXED TOKEN THAT IS NOT A COMPLETE LINK FAILS CLOSED. The
    # producers capture a stamp target with `\S+`, so a label containing a
    # SPACE -- ``[`01-boot-platform/TODO-07 §9`](...)``, a shape the live corpus
    # writes -- is split at that space and only the fragment
    # ``[`01-boot-platform/TODO-07`` reaches here. Falling through to generic
    # path resolution then answered with the file named in the LABEL rather than
    # the one named in the destination, which is a wrong binding reported as a
    # clean edge (Codex re-adversarial, section 26 review round 3, [high]).
    # Refusing is the honest verdict: the token is a fragment, and a fragment
    # names nothing. Fixing the producers to capture a whole link is the other
    # half and is filed separately -- it belongs to the cache producer, not here.
    if link_m is None and target.startswith("["):
        return None
    if link_m:
        # An empty or fragment-only destination names no FILE. `[x](#anchor)`
        # is a same-document jump, which no file-level resolution can answer,
        # so it refuses rather than falling through to be parsed as a path.
        target = (link_m.group("url") or "").strip().strip("`").strip()
        if not target or target.startswith("#"):
            return None
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
    # 3. Same-domain compact: T17 -- resolved against source_file's own
    # DIRECTORY, not against the two digits in front of it. "Same domain" means
    # the file next to this one; reading `[:2]` made a `T02` written in
    # `14-alpha` resolve to a file in `14-beta` (section 26).
    m = COMPACT_T_RE.match(target)
    if m:
        src_dir = _src_dir(source_file)
        if src_dir:
            return _dirnum_lookup(path_index, src_dir, int(m.group("num")))
        return None
    # 3a. Domain-only compact: D14 -- resolve to the domain's INDEX.md.
    # Authors use `D14` to reference an entire domain's scope; the INDEX.md
    # is the canonical landing page. Must come after D<n>T<n> (step 2) so
    # `D02T19` doesn't match the D-only branch.
    #
    # ONE DIRECTORY OR NONE. This scanned `by_dn` for the first entry whose code
    # matched and returned that file's INDEX.md sibling, so with two directories
    # sharing a code the answer was whichever node the cache listed first --
    # `D14` gave alpha or beta depending on row order alone.
    m = COMPACT_D_RE.match(target)
    if m:
        return _code_index_path(path_index, m.group("dom"))
    # 4. Full domain path: 02-kernel-core/TODO-19 (with or without -name, .md)
    #
    # THE DIRECTORY THE AUTHOR WROTE IS THE LOOKUP KEY. Both this branch and 4a
    # captured only the two-digit prefix and threw `-kernel-core` away, so an
    # explicitly directory-qualified reference was answered by ANY directory
    # sharing the code -- `14-alpha/TODO-02` returned `todo/14-beta/TODO-02-b.md`
    # in both node orders. Not ambiguity: the reference said exactly which
    # directory it meant and was answered with a different one.
    m = DOMAIN_PATH_RE.match(target)
    if m:
        return _dirnum_lookup(path_index, m.group("dir"), int(m.group("num")))
    # 4a. Full domain path with -name[.md] suffix: 02-kernel-core/TODO-19-foo
    # or 02-kernel-core/TODO-19-foo.md. Common surface form in Inputs XREFs
    # and stamp links.
    #
    # THE SLUG IS PART OF THE REFERENCE, so this resolves the exact stem INSIDE
    # the named directory and refuses otherwise. A `(directory, number)` lookup
    # threw the authored filename away, so a target naming no existing file
    # bound to whichever TODO carried that number:
    # `12-user-platform-sdk/TODO-03-kernel-libraries.md` was answered by
    # `TODO-03-elf-reloc-kernel-modules.md`, and
    # `16-architecture-ports/TODO-01-multi-arch-port.md` by
    # `TODO-01-arch-abstraction-layer.md` -- while the file each one NAMES
    # exists, in another directory (Codex re-adversarial, section 26 round 4,
    # [high]).
    #
    # NO NUMBER FALLBACK, which is the rule this file already applies to a
    # spelled LETTER target: a spelled-out target is decided by its exact match
    # count, zero included, because letting one fall through is how a typo, a
    # rename or a deleted target silently redirects every XREF naming it while
    # `check_stale_xref` reports nothing (section 22 round 6). Measured over the
    # live corpus before adopting it: exactly 4 references name a stem their
    # directory does not carry, all 4 genuinely stale, and they are repaired in
    # this section rather than papered over.
    m = re.match(r"^(?P<dir>\d{2}-[a-z0-9-]+)/TODO-(?P<num>\d{1,2})-[a-z0-9-]+(?:\.md)?$", target)
    if m:
        want_dir = m.group("dir")
        stem = target[:-3] if target.endswith(".md") else target
        inside = [fp for fp in path_index.get("stem_paths", {})
                  .get(stem.rsplit("/", 1)[-1], ())
                  if _path_dir(fp) == want_dir]
        if len(inside) != 1 or _collides(path_index, inside[0]):
            return None
        return inside[0]
    # 4b. Domain-only reference (`05-storage-filesystems`, with or without a
    # trailing `/INDEX.md`). Authors use this to mean "the whole domain"
    # when the XREF is scope-level rather than file-level; resolve to the
    # domain's INDEX.md as the canonical landing page.
    #
    # EXACT DIRECTORY MEMBERSHIP, never a code scan. This matched the code out
    # of the directory name and then returned the first `by_dn` entry sharing
    # it, so the explicitly-named `14-alpha` resolved to `todo/14-beta/INDEX.md`
    # whenever beta was listed first -- the author named a directory and got its
    # neighbour. A directory nothing indexes now refuses instead of borrowing a
    # sibling's existence.
    m = re.match(r"^(?P<dir>\d{2}-[a-z0-9-]+)(?:/INDEX\.md)?$", target)
    if m:
        return _dir_index_path(path_index, m.group("dir"))
    # 4c. Letter-numbered TODO file (TODO-A, TODO-A-Win32k-Shadow-...).
    # A small set of catalog/master-table files use a letter suffix instead
    # of a numeric one. Match by filename stem or full domain path form.
    #
    # AND IT QUALIFIES BY DIRECTORY, not by code. This branch captured only the
    # two digits, so `14-alpha/TODO-A-beta.md` -- a reference that names its
    # directory AND spells its stem in full -- was answered by `14-beta`'s file
    # in both node orders (Codex adversarial, section 26, [high]). Every other
    # directory-qualified form in this resolver was corrected; leaving the
    # letter form on the code would have kept one door open.
    letter_m = re.match(
        r"^(?:(?P<dir>\d{2}-[a-z0-9-]+)/)?TODO-(?P<letter>[A-Z])(?:-[A-Za-z0-9-]+)?(?:\.md)?$",
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
        if exact_paths and letter_m.group("dir"):
            # A directory qualifier narrows the candidates BEFORE the uniqueness
            # test -- otherwise `01-a/TODO-A-Shared.md` is refused as ambiguous
            # on the strength of a same-named file in another domain, which is
            # exactly the reference form that disambiguates it.
            want = letter_m.group("dir")
            exact_paths = [fp for fp in exact_paths if _path_dir(fp) == want]
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
        want_dir = letter_m.group("dir")
        matches = []
        for fp in (path_index.get("sorted_paths")
                   or sorted({q for ps in path_index.get("stem_paths", {}).values()
                              for q in ps} or _all_paths(path_index))):
            tail = fp.rsplit("/", 1)[-1]
            if not (tail.startswith(stem_fragment + "-")
                    or tail == stem_fragment + ".md"):
                continue
            if want_dir is None or want_dir == _path_dir(fp):
                matches.append(fp)
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1 and want_dir is None:
            # SAME-DIRECTORY WINS, exactly as bare `TODO-NN` resolves against the
            # source file's own folder in branch 5. Three `TODO-A-*` files exist
            # across different domains, so an unqualified `TODO-A` was binding
            # to whichever one `set()` iteration yielded first -- arbitrary, and
            # silently so. Preferring the source's own directory makes the common
            # in-folder reference deterministic; anything else stays ambiguous
            # and surfaces as an unresolved XREF the author must qualify.
            src_dir = _src_dir(source_file)
            same = [fp for fp in matches if _path_dir(fp) == src_dir]
            if len(same) == 1:
                return same[0]
        # Zero matches, or an ambiguous reference the author must qualify.
        return None
    # 5. Bare TODO-NN -- resolved against source_file's own DIRECTORY, same
    # rule and same reason as branch 3.
    m = TODO_NN_RE.match(target)
    if m:
        src_dir = _src_dir(source_file)
        if src_dir:
            return _dirnum_lookup(path_index, src_dir, int(m.group("num")))
        return None
    # 5a. TODO-NN-name[.md] -- a FULLY SPELLED filename, so the SLUG decides it
    # and the number is only the fallback.
    #
    # This matched the token and then resolved `(source directory, NN)`,
    # throwing the authored slug away -- which also made the exact-stem branches
    # 6 and 7 below unreachable for every numeric filename, since this branch
    # returns first. Live consequence, not a constructed one:
    # `TODO-05-win32-file-io-api.md`, cited from
    # `todo/02-kernel-core/TODO-21-process-model-extensions.md`, resolved to
    # `todo/02-kernel-core/TODO-05-object-manager.md` -- a file-I/O reference
    # answered by the object manager, because both are number 5 and the slug was
    # never consulted (Codex re-adversarial, section 26 round 3, [high]).
    #
    # Same rule the letter branch already applies to a spelled stem, and the
    # same as branch 4a above: an exact match decides, ambiguity prefers the
    # source's own directory and otherwise refuses, and there is NO number
    # fallback -- a spelled filename no file carries is a stale reference, which
    # is exactly what `check_stale_xref` exists to say. Measured: 0 live bare
    # spelled targets rely on such a fallback.
    m = re.match(r"^TODO-(?P<num>\d{1,2})-[a-z0-9-]+(?:\.md)?$", target)
    if m:
        stem = target[:-3] if target.endswith(".md") else target
        cands = path_index.get("stem_paths", {}).get(stem)
        if not cands:
            return None
        if len(cands) == 1:
            return None if _collides(path_index, cands[0]) else cands[0]
        src_dir = _src_dir(source_file)
        same = [fp for fp in cands if _path_dir(fp) == src_dir]
        if len(same) != 1 or _collides(path_index, same[0]):
            return None
        return same[0]
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
                    # Exact path membership is still subject to the collision
                    # rule -- otherwise `./TODO-01-first.md` answers a target
                    # that `19-x/TODO-01-first.md` refuses.
                    return None if _collides(path_index, rel_path) else rel_path
                # INDEX.md relative forms (../07-networking/INDEX.md).
                # Return the normalized INDEX.md path if it names a live
                # domain folder.
                #
                # THE NAMED DIRECTORY MUST EXIST, not merely its code. This
                # confirmed that SOME directory shared the two-digit prefix and
                # then returned the authored path regardless, so
                # `../14-ghost/INDEX.md` resolved happily on the strength of
                # `14-alpha` existing -- a dangling reference reported as live,
                # which is the exact failure `check_stale_xref` is for.
                if rel_path.endswith("/INDEX.md"):
                    dom_dir = rel_path.split("/")[1]
                    if re.match(r"^\d{2}-", dom_dir) \
                            and _dir_index_path(path_index, dom_dir) == rel_path:
                        return rel_path
                # Path does not include the slug or .md suffix: try to map
                # the relative form back to (domain, num) and look up in
                # by_dn. Handles `../02-kernel-core/TODO-03` (no slug, no
                # .md). Normalized `rel_path` would be something like
                # `todo/02-kernel-core/TODO-03`.
                tail = rel_path.rsplit("/", 1)[-1]
                rel_parts = rel_path.split("/")
                # Keyed on the NORMALIZED DIRECTORY, not its code: the relative
                # form spells the directory out just as branch 4 does, so
                # `../14-alpha/TODO-02` must not be answered by `14-beta`.
                #
                # SLUGLESS ONLY. `^TODO-(\d{1,2})(?:-|\.md|$)` also matches a
                # fully spelled `TODO-05-win32-file-io-api.md`, so a relative
                # target carrying a slug reached this number lookup and had its
                # filename discarded -- the branch-4a defect by another route.
                # A spelled relative target has already been decided above by
                # exact path membership; reaching here means no such file
                # exists, which is a stale reference, not a number to guess at.
                if len(rel_parts) >= 3 and rel_parts[0] == "todo":
                    dom_dir = rel_parts[1]
                    # A SPELLED tail WITHOUT `.md` is still spelled. The exact
                    # path check above only matches a target carrying the
                    # suffix, so `./TODO-01-a` fell through to the slugless
                    # number lookup, missed it, and returned None -- while the
                    # identical `TODO-01-a` and `14-alpha/TODO-01-a` both
                    # resolved. Same rule as branch 4a, applied to the
                    # normalized directory (Codex adversarial, section 26
                    # review, [medium]).
                    #
                    # AND IT IS THE WHOLE NORMALIZED PATH THAT MUST EXIST, not
                    # its first and last components. Matching on
                    # `(dom_dir, tail)` discarded every directory in between,
                    # so `./ghost/TODO-01-a` -- normalizing to
                    # `todo/14-alpha/ghost/TODO-01-a`, which is nothing --
                    # resolved to `todo/14-alpha/TODO-01-a.md` and reported no
                    # stale-XREF finding (Codex re-adversarial, section 26
                    # review round 2, [high]). Adding the suffix and asking for
                    # the exact path keeps the depth information the reference
                    # actually carries.
                    if re.match(r"^TODO-\d{1,2}-[a-z0-9-]+$", tail):
                        cand = rel_path + ".md"
                        if cand not in _all_paths(path_index) \
                                or _collides(path_index, cand):
                            return None
                        return cand
                    num_m = re.match(r"^TODO-(\d{1,2})(?:\.md)?$", tail)
                    if re.match(r"^\d{2}-", dom_dir) and num_m:
                        return _dirnum_lookup(path_index, dom_dir,
                                              int(num_m.group(1)))
        except (OSError, ValueError):
            pass
    # 9. Already a path-like form (00-domain/TODO-NN-name.md). Collision rule
    # applies here too -- this is the last spelling of the same target.
    if target in _all_paths(path_index):
        return None if _collides(path_index, target) else target
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
    `(updates_total, ambiguities, unresolvable_targets, report_lines)`. Codex
    pass 6 M1 contract: FAIL on non-unique item-name match (no
    first-match-wins); never invent a new item; require explicit --write to
    actually mutate. Codex pass 7 H2: ambiguity must surface as a non-zero exit
    so CI cannot silently treat a refused rewrite as success.

    NOTHING IS PRINTED FROM HERE. Every diagnostic is accumulated into
    `report_lines` and handed back for the caller to publish -- after the
    dry-run corpus re-verification, or after the write set has been committed.
    Emitting inline meant a run that later refused at rc 2 had already told the
    operator what it found (Codex adversarial, section 23, [medium]).
    """
    updates_total = 0
    ambiguities = 0
    unresolvable_targets = 0
    report: list = []
    pending_writes: list = []
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
                    # An XREF target outside the cache's node set, so the
                    # startup snapshot never read it. Same bounded, fail-closed
                    # reader as the snapshot itself: an unreadable target here
                    # used to escape as an OSError traceback at rc 1, and a
                    # rewrite decided from a partially-read file is worse than
                    # no rewrite.
                    try:
                        target_text = cache_schema.read_corpus_file(
                            repo_root / target_file).decode("utf-8")
                    except (cache_schema.CacheSchemaError, OSError,
                            ValueError) as exc:
                        _refuse(f"cannot read XREF target {target_file} while "
                                f"re-resolving line numbers in {rel}: {exc}")
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
                    report.append(
                        f"[validate.py] FAIL fix-line-numbers: ambiguous item_name "
                        f"{name!r} matches lines {hits} in {target_file} "
                        f"(stamp at {rel}); refusing to rewrite"
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
                    report.append(
                        f"[validate.py] {'WRITE' if write else 'DRY-RUN'} "
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
            # STAGED, NOT WRITTEN. See `_commit_rewrites` for why nothing may
            # touch the disk until every destination has been checked.
            pending_writes.append((rel, text, new_text))
    if write and pending_writes:
        # `files_modified` used to be accumulated here and never read by
        # anything; removed rather than carried forward into the two-phase
        # commit, where a second bookkeeping copy of the write set would be one
        # more thing to keep in step with the real one.
        _commit_rewrites(repo_root, pending_writes)
    if ambiguities > 0 and write:
        report.append(
            f"[validate.py] {ambiguities} ambiguous item_name(s) refused; "
            "fix duplicates manually then re-run"
        )
    if not quiet:
        report.append(
            f"[validate.py] fix-line-numbers: {updates_total} update(s), "
            f"{ambiguities} ambiguous, {unresolvable_targets} unresolvable target(s); "
            f"mode={'WRITE' if write else 'DRY-RUN'}"
        )
    return (updates_total, ambiguities, unresolvable_targets, report)


def _commit_rewrites(repo_root: Path, pending: list) -> None:
    """Apply every staged rewrite, or none of them.

    TWO PHASES, because one phase published a partial repair. The rewrite used
    to happen inside the per-node loop, so a destination that had moved was
    discovered only when its turn came -- after earlier files were already
    rewritten and their DRY-RUN/WRITE lines already printed. The run then
    refused at rc 2 having both mutated the tree and announced results, which
    is precisely the "no verdict means say nothing" contract the check path
    holds to (Codex adversarial, section 23, [medium]).

    Phase 1 re-reads and compares EVERY destination against the snapshot text
    it was transformed from. Any mismatch or unreadable file refuses before a
    single byte is written. Phase 2 then replaces each file.

    EACH REPLACEMENT IS ATOMIC. `write_text` truncates the destination in
    place, so a failure mid-write leaves a half-written TODO -- worse than the
    lost update it was guarding against. Writing a sibling temp file and
    `os.replace`-ing it means a reader ever only sees the old file or the new
    one.

    ALL-OR-NONE IS NOT CLAIMED FOR PHASE 3, because it cannot be delivered.
    Every replacement is staged and fsynced first, so by the time any directory
    entry moves the only remaining operation per file is `os.replace` within one
    directory -- but a failure part-way through a multi-file batch still leaves
    earlier files replaced. Rolling those back would mean writing MORE files on
    the path where writing is already failing. So the batch reports exactly
    which files it committed instead of asserting an atomicity it does not have
    (Codex adversarial, section 23 review, [high]).

    A RESIDUAL WINDOW REMAINS between the phase-1 compare and the phase-3
    replace, and it is documented rather than closed. Closing it properly needs
    an exclusion protocol -- a lock file every writer agrees to take -- and
    nothing else in this repo takes one, so a lock here would be ceremony that
    excludes nobody while implying it does. What is bounded is the harm: no
    partial file is ever observable, and the window is microseconds rather than
    the whole walk.
    """
    # PHASE 1 -- verify every destination, and establish its IDENTITY.
    #
    # `lstat`, not `stat`: the corpus deliberately accepts a symlink pointing at
    # a regular TODO file, and `os.replace` would swap the LINK ENTRY for a
    # regular file, silently converting a tracked link into a copy. Repairing
    # through a link is ambiguous enough that refusing is the honest answer.
    #
    # `(st_dev, st_ino)` deduplication catches the other aliasing case: two
    # cache nodes naming one inode both pass the compare, and their `new_text`
    # can differ because line resolution is relative to the SOURCE path -- so
    # the second replacement would silently discard the first (Codex
    # adversarial, section 23 review, [high]).
    staged: list = []
    by_inode: dict = {}
    todo_root = Path(os.path.realpath(repo_root / "todo"))
    for rel, old_text, new_text in pending:
        dest = repo_root / rel
        # CONTAINMENT IS RE-ESTABLISHED HERE, not inherited. `cache_schema`
        # refuses a node path that escapes the corpus, but that is a different
        # module and this is the only code in the tool that WRITES -- so the
        # last thing standing between an untrusted cache and someone's files
        # should not depend on another module having been called first. The
        # resolve also catches an ancestor SYMLINK, which a purely textual path
        # rule cannot see.
        try:
            resolved = Path(os.path.realpath(dest))
            resolved.relative_to(todo_root)
        except (OSError, ValueError):
            _refuse(f"{rel} resolves outside the corpus at {todo_root}; "
                    f"nothing was written")
        try:
            st = os.lstat(dest)
        except OSError as exc:
            _refuse(f"cannot stat {rel} before rewriting it, so no rewrite is "
                    f"safe; nothing was written: {exc}")
        if stat.S_ISLNK(st.st_mode):
            _refuse(f"{rel} is a symlink; rewriting it would replace the link "
                    f"with a regular file. Nothing was written -- edit the "
                    f"link's target directly")
        # LINK COUNT, not a duplicate scan. `os.replace` swaps ONE directory
        # entry and breaks the hard link, so every OTHER name for this inode
        # keeps the old content and goes stale while the command reports
        # success. Two attempts got this wrong before landing here:
        #
        #  - Dropping a duplicate whose derived text matched. Deduplicating
        #    REPLACEMENTS is not updating every PATHNAME.
        #  - Refusing when two PENDING destinations shared an inode. That map is
        #    built from `pending_writes` ALONE, so an alias that needs no
        #    rewrite of its own -- or one outside the cached corpus entirely --
        #    is never seen, and exactly one name reaches this loop (Codex
        #    re-adversarial round 2, section 23 review, [high]).
        #
        # `st_nlink` counts EVERY name for this inode, whether or not this run
        # knows about them, which is what the pending-set map could not do.
        #
        # IT DOES NOT REPLACE THAT MAP, THOUGH, and deleting it was a third
        # mistake: two pending paths can reach ONE directory entry through
        # bind-mounted parents while each reports `st_nlink == 1`. Publishing
        # the first then changes what the second names, and the second is
        # refused only after the first is already committed. The two guards
        # cover different aliasing mechanisms, so both stay (Codex
        # re-adversarial round 3, section 23 review, [medium]).
        ident = (st.st_dev, st.st_ino)
        if ident in by_inode:
            _refuse(f"{rel} and {by_inode[ident]} name the same file through "
                    f"different paths; rewriting one silently changes the "
                    f"other. Nothing was written")
        by_inode[ident] = rel
        if st.st_nlink > 1:
            _refuse(f"{rel} has {st.st_nlink} names (hard link); rewriting it "
                    f"would break the link and leave every other name on the "
                    f"old content. Nothing was written -- replace the link "
                    f"with a copy, or repair this stamp by hand")
        # A FILE BIND MOUNT is a third aliasing mechanism, and it defeats both
        # guards above: it increments no link count and puts no second path in
        # the pending set, yet replacing this directory entry leaves the bound
        # pathname pinned to the old inode (Codex re-adversarial round 4,
        # section 23 review, [high]). It needs no mount-table parsing to spot,
        # though -- a bind-mounted file is its own mount point, so its st_dev
        # differs from its parent directory's.
        #
        # WHAT THIS CANNOT SEE, stated precisely rather than narrowed twice and
        # still overclaimed. `st_dev` identifies the FILESYSTEM, not the mount
        # instance, so a bind mount whose source and target share a filesystem
        # keeps its parent's device number and passes -- Python's own
        # `os.path.ismount` documentation says as much, and `/snap` on this host
        # is a live example. Detecting those needs mount identity (statx mount
        # IDs, or parsing /proc/self/mountinfo), and an alias in another mount
        # NAMESPACE is not detectable at all.
        #
        # Deliberately NOT implemented here. The realistic hazard for a TODO
        # line-number fixer is a concurrent editor, which the content compare
        # covers; the guards below are worth keeping because they cost one stat
        # each, but building mount-table inspection into this tool would buy a
        # threat model this repo does not have. So the contract is: hard links
        # and cross-filesystem mounts are refused, same-filesystem bind mounts
        # and cross-namespace aliases are NOT detected (Codex re-adversarial
        # round 5, section 23 review, [high] -- recommendation to use statx
        # mount IDs declined, claim corrected instead).
        try:
            parent_dev = os.stat(dest.parent).st_dev
        except OSError as exc:
            _refuse(f"cannot stat the directory holding {rel}; nothing was "
                    f"written: {exc}")
        if st.st_dev != parent_dev:
            _refuse(f"{rel} is a mount point (bind-mounted file); rewriting it "
                    f"would replace the directory entry and leave every other "
                    f"name for it on the old content. Nothing was written")
        try:
            current = cache_schema.read_corpus_file(dest).decode("utf-8")
        except (cache_schema.CacheSchemaError, OSError, ValueError) as exc:
            _refuse(f"cannot re-read {rel} before rewriting it, so no rewrite "
                    f"is safe; nothing was written: {exc}")
        if current != old_text:
            _refuse(f"{rel} changed since it was read, so rewriting it would "
                    f"discard that edit; nothing was written. Re-run "
                    f"--fix-line-numbers against the current tree")
        staged.append((rel, dest, old_text, new_text,
                       stat.S_IMODE(st.st_mode), ident))

    # PHASE 2 -- materialise every replacement before ANY of them is published.
    #
    # `mkstemp` rather than a fixed `<dest>.validate-tmp`: that name was
    # predictable and was opened without O_EXCL or O_NOFOLLOW, so a pre-existing
    # sibling was truncated and a planted symlink was FOLLOWED -- truncating
    # whatever it pointed at, and then `os.replace` moved the symlink over the
    # TODO. `mkstemp` opens O_CREAT|O_EXCL|O_NOFOLLOW at a unique name (Codex
    # adversarial, section 23 review, [high]). The temp files are removed in a
    # `finally` covering every exception path, not just OSError.
    # PAIRS ARE IMMUTABLE; the outstanding set is what changes. An earlier
    # revision iterated `zip(staged, tmps)` while `tmps.remove(...)` ran inside
    # the loop -- and `zip` holds a positional iterator, so removing element 0
    # shifted the list and the SECOND destination got paired with the THIRD
    # destination's staged text. A two-file repair silently wrote C's content
    # over B and returned success. Every fixture had exactly one destination, so
    # nothing saw it (Codex re-adversarial, section 23 review, [high]).
    pairs: list = []
    outstanding: set = set()
    try:
        for entry in staged:
            rel, dest, _old_text, new_text, mode, _ident = entry
            # INSIDE the handler. `mkstemp` sat outside it, so ENOSPC,
            # EDQUOT, EACCES or descriptor exhaustion escaped `main()` as a
            # traceback and exited 1 -- the code this validator reserves for a
            # COMPLETED graph verdict. A disk-full repair would have sent
            # someone to fix a graph that was never evaluated, which is the
            # exact confusion this section exists to end (Codex re-adversarial
            # round 3, section 23 review, [high]).
            try:
                fd, tmp_name = tempfile.mkstemp(
                    dir=str(dest.parent), prefix=dest.name + ".",
                    suffix=".tmp")
            except OSError as exc:
                _refuse(f"cannot stage the rewrite of {rel}; nothing was "
                        f"written: {exc}")
            outstanding.add(tmp_name)
            try:
                with os.fdopen(fd, "w", encoding="utf-8") as fh:
                    # `fchmod` on the descriptor, and INSIDE the refusing
                    # handler. `mkstemp` creates at 0600, so swallowing a mode
                    # failure published a replacement other users could not
                    # read -- a repair that returns 0 having made a shared TODO
                    # private is materially broken, not cosmetically imperfect
                    # (Codex re-adversarial round 4, section 23 review,
                    # [medium]).
                    os.fchmod(fh.fileno(), mode)
                    fh.write(new_text)
                    fh.flush()
                    os.fsync(fh.fileno())
            except OSError as exc:
                _refuse(f"failed to stage the rewrite of {rel}; nothing was "
                        f"written: {exc}")
            pairs.append((entry, tmp_name))

        # PHASE 3 -- publish, RE-VERIFYING each destination immediately before
        # its replace. Phase 1's comparison for the first file happens before
        # every later file is compared and staged, so for a batch the exposure
        # was the whole staging pass, not "microseconds" -- long enough for an
        # editor's atomic save to land and be overwritten, or for a destination
        # to become a symlink after its lstat (Codex re-adversarial, section 23
        # review, [high]). The re-check costs one lstat plus one read per file
        # and shrinks the window to the gap before a single `os.replace`.
        committed: list = []
        for (rel, dest, old_text, _new_text, mode, ident), tmp_name in pairs:
            done = ", ".join(committed) if committed else "none"
            try:
                st_now = os.lstat(dest)
            except OSError as exc:
                _refuse(f"{rel} became unreadable while the batch was staged: "
                        f"{exc}. ALREADY REWRITTEN: {done}")
            if stat.S_ISLNK(st_now.st_mode) or (st_now.st_dev,
                                                st_now.st_ino) != ident:
                _refuse(f"{rel} was replaced while the batch was staged, so "
                        f"rewriting it would discard that change. ALREADY "
                        f"REWRITTEN: {done}")
            # The link count is re-read too, not carried from phase 1. A hard
            # link created DURING staging leaves the device, inode, contents
            # and file type all unchanged, so every other check here passes
            # while `os.replace` would still break the new link and strand it
            # on the old contents (Codex re-adversarial round 3, section 23
            # review, [high]). This narrows the race to the gap before a single
            # `os.replace`; closing it entirely needs an exclusion protocol
            # every writer takes, which is the same boundary documented above.
            if st_now.st_nlink != 1:
                _refuse(f"{rel} gained a second name while the batch was "
                        f"staged; rewriting it would leave that name on the "
                        f"old content. ALREADY REWRITTEN: {done}")
            # The MODE is state as well, and phase 2 baked the phase-1 value
            # into the staged inode. A chmod during staging changes none of the
            # fields checked above, so the replacement would silently restore
            # the old permissions -- removing access, or restoring access
            # somebody had just revoked (Codex re-adversarial round 5, section
            # 23 review, [medium]).
            if stat.S_IMODE(st_now.st_mode) != mode:
                _refuse(f"the permissions on {rel} changed while the batch was "
                        f"staged; rewriting it would restore the old mode. "
                        f"ALREADY REWRITTEN: {done}")
            try:
                now = cache_schema.read_corpus_file(dest).decode("utf-8")
            except (cache_schema.CacheSchemaError, OSError, ValueError) as exc:
                _refuse(f"cannot re-read {rel} at publish time: {exc}. "
                        f"ALREADY REWRITTEN: {done}")
            if now != old_text:
                _refuse(f"{rel} changed while the batch was staged, so "
                        f"rewriting it would discard that edit. ALREADY "
                        f"REWRITTEN: {done}")
            try:
                os.replace(tmp_name, dest)
            except OSError as exc:
                _refuse(f"failed to publish the rewrite of {rel}: {exc}. "
                        f"ALREADY REWRITTEN before this failure: {done}. The "
                        f"remaining files are untouched")
            outstanding.discard(tmp_name)
            committed.append(rel)
    finally:
        # Runs on SystemExit from `_refuse` too, so a refusal at any phase
        # leaves no staging file behind.
        for leftover in outstanding:
            try:
                os.unlink(leftover)
            except OSError:
                pass


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


def check_duplicate_resolver_key(nodes: list) -> list:
    """The DERIVED `(domain, TODO-number)` identity, checked the same way the
    frontmatter `id` is.

    `cache_schema` refuses a cache with two nodes on one resolver key, because
    every reader that keys a dict on it silently rebinds the loser's edges to
    the winner. `validate.py` deliberately does NOT take that refusal -- it
    reports identity collisions rather than declining to run -- but it was only
    reporting HALF of them: duplicate `id` had a check and the derived key did
    not. `build_path_index` records the collision and `resolve_xref_target`
    then refuses to resolve it, so it surfaced only when some XREF happened to
    use the ambiguous compact form. With no such reference, or only exact
    relative-path ones, all the other checks pass and this exits 0 -- on a cache
    `query.py` and the MCP server refuse outright.

    That divergence is the defect: the canonical gate certified a graph the
    query surface could not answer from. Reported at rc 1, where a corpus
    problem belongs, so both tools now agree on WHAT is wrong and differ only in
    what they do about it (Codex adversarial, section 23, [high]).
    """
    findings: list = []
    by_key: dict = {}
    for n in nodes:
        rel = n.get("file_path")
        if not rel:
            continue
        key = cache_schema.resolver_key(rel)
        if key is None:
            continue
        by_key.setdefault(key, []).append(rel)
    for key, paths in sorted(by_key.items()):
        if len(paths) <= 1:
            continue
        dom, num = key
        findings.append(Finding(
            "duplicate-resolver-key", paths[0],
            f"domain {dom} TODO-{num} is claimed by {len(paths)} files: "
            f"{', '.join(sorted(paths))}; a compact reference to it binds to "
            f"whichever the cache lists last, and every query reader refuses "
            f"this cache outright",
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


def diff_caches(baseline_path: Path, current_nodes: list, repo_root: Path,
                quiet: bool) -> list:
    """Return a list of Finding objects describing regressions the PR
    introduced: added orphans, new broken backlinks (ids present in
    baseline but missing in current), status downgrades (done -> active,
    etc), and newly stale XREFs. Each finding is tagged under a single
    `graph-delta` check name so it surfaces consistently in CI output.

    THE BASELINE IS THE SECOND CACHE THIS TOOL READS, and it was the one nobody
    had looked at: it went in raw, guarded only into a `graph-delta` FAIL. That
    guard is the wrong shape twice over. It catches unreadable-or-not-a-list and
    nothing else, so a schema-valid-looking baseline with a scalar `depends_on`
    or a non-dict stamp XREF still reached the walk. And where it does fire, it
    converts an infrastructure problem into a FINDING -- "this PR made the graph
    worse" -- which a human then acts on. A false delta nobody authored is worse
    than a refusal, because the refusal is obviously about the tool.

    So the baseline is now validated under its own profile and a failure is a
    REFUSAL at rc 2, never a delta. The profile differs from the current
    cache's on the axis the caller-declared-profiles API exists for: freshness
    is DISABLED (`check_stale=False`). The baseline is deliberately old and is
    not even built from this checkout's corpus, so a current-corpus staleness
    verdict against it would be meaningless rather than strict. The generation
    binding on the bytes actually read is unconditional and is retained.
    """
    findings: list = []
    try:
        baseline_nodes, _base_info = cache_schema.load_and_validate(
            baseline_path, repo_root / "todo", check_stale=False,
            # FRESHNESS OFF IS WHAT REQUIRES THE BINDING (section 25).
            # `check_cache_format` derives that: nothing else in this read
            # would ever look at the binding, and a baseline with no producer
            # identity is exactly the artifact section 23 could only ASK the
            # caller not to import. An absent one is now a refusal.
            profile=cache_schema.PROFILE_BASELINE)
    except cache_schema.CacheSchemaError as exc:
        # THE WORDING NOW REPORTS A CHECK RATHER THAN ASKING FOR A FAVOUR.
        # Until section 25 this path could only say "regenerate the baseline
        # with the current build.py", because shape was all it had: a field
        # that kept its type and changed its meaning passed, and produced
        # deltas nobody authored. `load_and_validate` verifies the artifact's
        # producer-contract identity BEFORE any subtree walk, so a baseline
        # from a different contract now refuses as LEGACY_FORMAT naming both
        # versions -- and the ask has become an assertion the code backs.
        #
        # A REFUSAL HERE NEVER REBUILDS. `--diff` names somebody else's
        # artifact by definition (CI builds it from the PR base SHA), so the
        # recovery path deliberately does not reach it: the bytes are left
        # exactly as they were and the caller is told what they hold.
        _refuse(f"--diff baseline at {baseline_path} cannot be trusted, so no "
                f"graph delta is reported [{exc.reason}]: {exc}. The baseline "
                f"is left untouched; rebuild it from the base revision with "
                f"this checkout's build.py (which is what the CI job does) "
                f"rather than importing an artifact written by another "
                f"producer contract")

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
    nodes, cache_info = load_or_rebuild_cache(cache_path, repo_root, args.quiet)
    todo_root = repo_root / "todo"

    snapshot = load_file_snapshot(repo_root, nodes)
    id_index = build_id_index(nodes)
    path_index = build_path_index(nodes)

    if args.fix_line_numbers:
        # AN AMBIGUOUS GRAPH CANNOT BE REPAIRED, and repair runs long before
        # the duplicate-id check below would report one. `build_id_index` is
        # last-wins, `resolve_xref_target` accepts frontmatter ids, and
        # PROFILE_VALIDATE deliberately permits duplicates so the check can
        # report them -- so `--fix-line-numbers --write` could resolve a stamp
        # against the WRONG file of a colliding pair, compute a line from it,
        # rewrite the stamp and exit 0 (Codex re-adversarial round 4, section
        # 23 review, [medium]). Refusing is right rather than merely safe: the
        # repair's whole job is to make stamp line numbers true, and it cannot
        # do that while it does not know which file a stamp names.
        id_collisions = check_duplicate_id(nodes)
        if id_collisions:
            _refuse("cannot re-resolve line numbers while frontmatter ids "
                    "collide -- a stamp naming a duplicated id would be "
                    "rewritten against whichever file the cache lists last. "
                    "Nothing was written. Offending: "
                    + "; ".join(f.detail for f in id_collisions))
        _updates, ambig, _unres, report = fix_line_numbers(
            nodes, snapshot, id_index, path_index,
            repo_root, write=args.write, quiet=args.quiet,
        )
        # REPAIR MODE IS BOUND DIFFERENTLY FROM THE CHECK WALK, deliberately.
        # A dry run reaches a verdict about a corpus it only read, so it takes
        # the same post-walk re-verification the nine checks take below. A
        # --write run CANNOT: it changes the corpus itself, so a global
        # re-verification would report the tool's own edits as interference and
        # fail every successful repair. Its binding is per-destination instead,
        # inside `fix_line_numbers`: each file's bytes are compared against the
        # snapshot they were transformed from immediately before the replace,
        # and a mismatch refuses rather than overwrites (Codex design review,
        # section 23, [high]).
        if not args.write:
            _reverify_corpus(todo_root, cache_info)
        # Published only now: in dry-run after the corpus binding closed, in
        # write mode after `_commit_rewrites` applied the whole set. Either way
        # a refusal above exits 2 having said nothing about the corpus.
        for line in report:
            sys.stderr.write(line + "\n")
        # Codex pass 7 H2: ambiguity is a hard failure. --write that refused
        # to rewrite because of non-unique item-name match must exit non-zero
        # so CI or hooks cannot silently treat a refused rewrite as success.
        if ambig > 0:
            return 1
        return 0

    # Run all 9 checks.
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
        ("duplicate-resolver-key", check_duplicate_resolver_key, (nodes,)),
    ]:
        findings = check_fn(*args_tuple)
        check_results.append((name, findings))
        all_findings.extend(findings)

    # BUFFER THE VERDICT, DO NOT STREAM IT. Every line below is collected and
    # emitted only after the post-walk corpus re-verification passes. Printing
    # as we go would put `[FAIL] stale-xref: ...` on a human's screen and THEN
    # refuse at rc 2 -- and the human acts on the findings, not on the exit
    # code. A run that reaches no verdict must say nothing about the corpus.
    out: list = []
    fail_count = 0
    warn_count = 0
    for name, findings in check_results:
        fails = [f for f in findings if f.severity == "FAIL"]
        warns = [f for f in findings if f.severity == "WARN"]
        if not fails and not warns:
            if not args.quiet:
                out.append(f"  [PASS] {name}")
        else:
            if fails:
                fail_count += len(fails)
                out.append(f"  [FAIL] {name}: {len(fails)} failure(s)")
                for f in fails:
                    out.append(f"    {f.format()}")
            if warns:
                warn_count += len(warns)
                if not args.quiet:
                    out.append(f"  [WARN] {name}: {len(warns)} warning(s)")
                    for f in warns:
                        out.append(f"    {f.format()}")

    # --diff: run graph-delta check against BASELINE. A delta finding
    # counts as a failure even if all 7 primary checks pass; this is
    # the "this PR makes the graph worse" signal for code review.
    delta_fail = 0
    if args.diff:
        baseline_path = Path(args.diff)
        if not baseline_path.is_absolute():
            baseline_path = (repo_root / baseline_path).resolve()
        deltas = diff_caches(baseline_path, nodes, repo_root, args.quiet)
        delta_fails = [f for f in deltas if f.severity == "FAIL"]
        if not delta_fails:
            if not args.quiet:
                out.append("  [PASS] graph-delta")
        else:
            delta_fail = len(delta_fails)
            out.append(f"  [FAIL] graph-delta: {delta_fail} regression(s) vs {baseline_path}")
            for f in delta_fails:
                out.append(f"    {f.format()}")
        fail_count += delta_fail

    # The walk is over; close the generation binding BEFORE any of it is
    # published. On refusal this exits 2 and `out` is discarded unprinted.
    _reverify_corpus(todo_root, cache_info)
    for line in out:
        print(line)

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
