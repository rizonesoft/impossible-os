#!/usr/bin/env bash
# ============================================================================
# test_build_gate.sh -- todo-graph build suite shard: Test 22, the section 16 identity gate.
#
# One of four shards scripts/test-tooling.sh starts together; the shared
# preamble, helpers and shard list are in test_build_lib.sh.
# ============================================================================

set -u
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/test_build_lib.sh" || exit 2

# The blocks here ran after Test 12's `cd "$REPO_ROOT"` in the single file,
# so they start where they always did.
cd "$REPO_ROOT" || exit 2

# The consumer-delegation fixtures below need a cache of the LIVE corpus. They
# read one this shard builds, never the real build/todo-cache.json, which
# test_build.sh deletes and rebuilds while this shard runs beside it.
GATE_LIVE_CACHE="$TMP_DIR/gate-live-cache.json"
python3 "$BUILD_PY" --quiet --output "$GATE_LIVE_CACHE" >/dev/null 2>&1 \
    || { echo "[test_build:gate] FAIL: build.py failed priming the live cache"; exit 2; }

# Tests 22a-22f: the section 16 identity GATE (scripts/todo-graph/
# identity-gate.sh).
#
# A wiring test that only proves the green path is the failure this roadmap
# keeps paying for, so every case below asserts a SPECIFIC outcome and the
# firing cases are mutation-checked: the same fixture is run with and without
# the deliberate resolver defect, and the test only counts if the mutation
# flips the verdict. A gate that cannot be made to fail is not a gate.
#
# The fixture builds a THROWAWAY CLONE with a pruned todo/ corpus rather than
# walking the live 1,626-ref corpus twice per case -- same invocation shape,
# seconds instead of minutes. The clone is seeded from the WORKING TREE copies
# of the closure files, so these tests exercise the code being changed rather
# than whatever is committed.
# ----------------------------------------------------------------------
GATE_REPO="$TMP_DIR/gate-repo"
# THE GROUP'S OWN RUNTIME CEILING STARTS HERE (section 59), bracketing every
# case from this line to the group's closing `fi`. `mono_ns`/`elapsed_ms`
# match the pattern Test 7 already uses for a wall-clock budget; the verdict
# and the chosen bar are recorded where the bracket closes.
G_S59_GROUP_START_NS="$(mono_ns)"
# Invoke the CLONE's copy, not the live one. identity-gate.sh derives its repo
# root from its own location and cd's there, so running the live script from
# inside the clone silently operates on the LIVE repo -- which is exactly what
# happened on the first run of these fixtures: the clone's SHAs did not exist
# there, the gate fell back to HEAD~1, found an unchanged closure and exited 0,
# so two firing cases "passed" without ever walking anything. Using the clone's
# own copy also means the fixture exercises the script as that tree ships it.
GATE_IN_CLONE="$GATE_REPO/scripts/todo-graph/identity-gate.sh"

# g_s54_wtgate <output-path>: a copy of the gate whose head-side PROTOCOL AND
# CLOSURE reads are put back on the LIVE WORKING TREE.
#
# ITS SCOPE IS THE READS, NOT THE WHOLE HEAD SIDE, and saying otherwise would
# overstate what every control built from it proves. The later phases -- the
# cache build, the producer differential, both resolver walks, the comparison --
# still execute from the materialized head tree in this copy, because those are
# not what any case below constructs a condition for. What it restores is
# exactly the surface those conditions live on.
#
# WHY IT EXISTS. Section 54 moved the head side to the commit, and a family of
# cases below built their condition in the WORKING TREE -- a removed protocol
# file, a symlinked ancestor directory, a poisoned bytecode cache, a file
# recreated after a committed deletion. Every one of those conditions is now
# correctly IGNORED, which is the point, but it means each of those cases has to
# assert the opposite of what it used to. An inverted assertion is easy to
# satisfy vacuously (a gate that died early for an unrelated reason also fails
# to emit the old token), so this copy is the control that proves the condition
# was really constructed: run it against the same tree and the old refusal comes
# back. It must be written INSIDE the clone -- the gate derives its repo root
# from its own location, so a copy anywhere else adjudicates a different tree.
# g_mutant_ok <source> <mutant> <needle>: is this mutant fit to draw a verdict
# from?
#
# DIFFERING FROM ITS SOURCE IS NECESSARY AND NOT SUFFICIENT, and the gap between
# those two is a live false-positive this file has now produced twice. A `sed`
# whose delimiter appears inside its own replacement is REJECTED, and the
# redirection has already truncated the output, so the mutant is ZERO BYTES --
# which `cmp -s` reports as "differs", and which `bash` runs as an empty script
# exiting 0 in silence. Every mutation case asserting the ABSENCE of a token
# then passes while the gate under test never ran at all. Found in the section 55
# 22eo case by its own failure, and then in 22el-wt and 22ef by looking for the
# shape (Codex adversarial, section 55 rounds 1 and 2).
#
# The needle is the text the substitution was supposed to INTRODUCE, so a sed
# that silently matched nothing is caught too.
g_mutant_ok() {
    ! cmp -s "$1" "$2" \
        && [ -s "$2" ] \
        && bash -n "$2" 2>/dev/null \
        && grep -q "$3" "$2"
}

g_s54_wtgate() {
    # THE SHAPE IT REWRITES MOVED IN SECTION 55, and the helper moved with it.
    # The head side no longer reads a full checkout at all before the
    # byte-identical exit: `proto_of` reads the narrow tree assembled from the
    # head commit's own blobs, and the closure reading is the OID carried by that
    # commit's tree entry, acquired for every member in one batch. So there are
    # two head-side reads left to put back on the working tree, not five -- the
    # protocol root, and the batched closure entries, which are overridden in
    # place immediately after they are acquired and before the first loop that
    # consumes them.
    #
    # THE ROOT IS NOW ADDRESSED THROUGH A DESCRIPTOR, so these anchors name
    # `$HEAD_MIN_ADDR` rather than `$HEAD_MIN`. Section 55 bound execution and
    # verification to one directory identity by passing `/proc/self/fd/N`; three
    # mutation builders anchored on the old spelling and correctly refused to
    # certify a mutant they had not actually built.
    # BOTH CLOSURE READINGS, not just the listing one. Section 55 acquires the
    # head closure twice by different plumbing -- `H_OID` from the tree listing
    # and `H_VOID` from the path expression -- and overriding only `H_OID` left
    # this control INCOHERENT: the measurement loop still read the COMMIT, so the
    # closure decision was the commit's while the decision loop's reading was the
    # working tree's, and every case built on this helper reached
    # `CLOSURE_MOVED_UNDER_GATE` for a reason that had nothing to do with what it
    # was testing. It went unnoticed because those cases assert the ABSENCE of a
    # protocol token, and an unrelated refusal satisfies that (Codex adversarial,
    # section 55 round 3, [medium]).
    #
    # AND THE OVERRIDE IS ANCHORED ON THE LAST OF THE TWO ASSIGNMENTS. Anchored
    # on the `H_OID=` line it was inserted BEFORE `H_VOID=`, so the very next
    # statement put the commit's values straight back over it -- the override ran
    # and was then undone, one line later.
    sed -e 's#^proto_of "\$HEAD_MIN_ADDR"$#proto_of "$REPO_ROOT"#' \
        -e 's#protocol_present_in_worktree "\$HEAD_MIN_ADDR"#protocol_present_in_worktree "$REPO_ROOT"#' \
        -e 's#^H_VOID=("\${ENT_VOID\[@\]}")$#&\
for _wti in "${!CLOSURE[@]}"; do\
    _wtf="${CLOSURE[$_wti]}"\
    if [ -f "$REPO_ROOT/$_wtf" ] \&\& [ ! -L "$REPO_ROOT/$_wtf" ]; then\
        H_OID[$_wti]="$(git hash-object "$REPO_ROOT/$_wtf" 2>/dev/null)"\
        H_VOID[$_wti]="${H_OID[$_wti]}"\
        H_PRESENT[$_wti]=1; H_TYPE[$_wti]=blob; H_MODE[$_wti]=100644; H_OBJ[$_wti]=1\
    elif [ -e "$REPO_ROOT/$_wtf" ] || [ -L "$REPO_ROOT/$_wtf" ]; then\
        H_PRESENT[$_wti]=1; H_TYPE[$_wti]=blob; H_MODE[$_wti]=120000; H_OBJ[$_wti]=1\
        H_OID[$_wti]="$(git hash-object "$REPO_ROOT/$_wtf" 2>/dev/null)"\
        H_VOID[$_wti]="${H_OID[$_wti]}"\
    else\
        H_PRESENT[$_wti]=0; H_OID[$_wti]=""; H_TYPE[$_wti]=""; H_MODE[$_wti]=""; H_OBJ[$_wti]=0\
        H_VOID[$_wti]=""\
    fi\
done#' \
        "$GATE_IN_CLONE" > "$1"
    # EVERY SUBSTITUTION IS VERIFIED, not just the first. Checking only
    # `proto_of` was an overstatement in a helper whose whole job is to be a
    # trustworthy control: a shape that moved anywhere else would leave that
    # read on the commit, and the control would then agree with the case it is
    # supposed to contradict while reporting that it had been built (Codex
    # consistency, section 54, [medium]).
    # AND THE MUTANT MUST DIFFER FROM ITS SOURCE. "No head-side read remains"
    # is also true of a file the sed never touched, so the absence check alone
    # would accept a copy of the unmutated gate the day one of these shapes
    # moves -- which is exactly how the cleanup-reversion mutation came to run
    # the original gate and report that its own fix did nothing.
    # NON-EMPTY AND PARSABLE TOO, not merely different and free of the old
    # shapes. A sed that rejects its own expression writes nothing, and an empty
    # file is both "different from its source" and "contains no head-side read"
    # -- so every check here passed on a mutant that cannot run. See
    # `g_mutant_ok` above for where that hole was found.
    ! cmp -s "$GATE_IN_CLONE" "$1" \
        && [ -s "$1" ] \
        && bash -n "$1" 2>/dev/null \
        && ! grep -qE '^proto_of "\$HEAD_MIN_ADDR"$|protocol_present_in_worktree "\$HEAD_MIN_ADDR"' "$1" \
        && grep -q '^for _wti in' "$1"
}

gate_seed_tree() {   # $1 = destination path
    # THE SEEDED TREE, BEFORE ANY PRUNING -- shared by `gate_seed` and by the
    # `22f8` control, which needs a tree identical to the real one in every
    # respect except that its closure derivation is crippled. Duplicating these
    # steps in the control is exactly the drift sections 17 and 18 each paid
    # for in this same function (a hardcoded closure list that stopped matching
    # the gate's own array, then a CLOSURE read that missed EXEC_CLOSURE's
    # expansion), so the control shares the code rather than mirroring it.
    rm -rf "$1" 2>/dev/null || true
    # HARDLINKED local clone. `--no-hardlinks` forced a physical copy of the
    # whole object store -- ~212 MiB in this checkout -- on every tooling run
    # (Codex perf, section 16). Hardlinks are safe here: git never rewrites an
    # existing object, the fixture only ADDS commits, and removing the clone
    # cannot affect the source through a hardlink.
    # No explicit --local: a path clone already hardlinks, and only the explicit flag
    # dies when it cannot. /tmp is tmpfs on Ubuntu 26.04, so that made every fixture
    # clone fail; left implicit, git falls back to a copy.
    git clone --quiet "$REPO_ROOT" "$1" >/dev/null 2>&1 || return 1
    (
        cd "$1" || exit 1
        git config user.email "test@example.invalid"
        git config user.name "identity gate fixture"
        # Seed with the working-tree closure files (the code under test).
        #
        # DERIVED FROM THE GATE'S OWN `CLOSURE` ARRAY, never a second hardcoded
        # copy of it. This list WAS hardcoded, and section 17 paid for it: the
        # closure grew `cache_schema.py`, this list did not, so the clone got a
        # snapshot importing a module that was not there. Every gate fixture
        # then failed with an import error at rc 1/3 -- which reads as "the gate
        # mis-fired" rather than "the fixture is missing a file". Extracting the
        # array literal means the two cannot drift again.
        #
        # BOTH arrays, and filtered to `scripts/` entries. Section 18 split the
        # list into EXEC_CLOSURE (can run code) plus a CLOSURE that expands it
        # with `"${EXEC_CLOSURE[@]}"`. Reading only CLOSURE then yielded ONE
        # real path plus the literal expansion text, so the clone was seeded
        # almost entirely from COMMITTED code and every fixture silently tested
        # the old gate -- the same drift this derivation exists to prevent,
        # wearing a different shape. The `scripts/` filter drops the expansion
        # token without needing to know what it expands to.
        while IFS= read -r f; do
            [ -n "$f" ] && [ -f "$REPO_ROOT/$f" ] && cp "$REPO_ROOT/$f" "$f"
        done < <(sed -n -e '/^EXEC_CLOSURE=(/,/^)/p' -e '/^CLOSURE=(/,/^)/p' \
                     "$REPO_ROOT/scripts/todo-graph/identity-gate.sh" \
                 | sed -n 's/^[[:space:]]*"\(scripts\/.*\)"[[:space:]]*$/\1/p' \
                 | sort -u)
        # check_stub_behind_stamp.py is DELIBERATELY absent from the closure
        # (it is neither executed by the walks nor an input to them), but the
        # fixture still needs the file present to exercise the lint side.
        cp "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
           scripts/lint/check_stub_behind_stamp.py 2>/dev/null || true
        # Prune the corpus: keep a handful of TODOs so the double walk is fast.
        keep=0
        while IFS= read -r t; do
            keep=$((keep + 1))
            [ "$keep" -le 6 ] && continue
            rm -f "$t"
        done < <(find todo -name 'TODO-*.md' -not -name 'TODO-00-INDEX.md' | sort)
    ) || return 1
    return 0
}

gate_reference_closure_prune() {
    # SECTION 59, ITEM 3 -- cut what a gate INVOCATION materializes.
    #
    # `gate_seed` clones once, so the clone is not the term that repeats; what
    # repeats is the fixture COMMIT'S TREE, which an invocation checks out
    # twice (base worktree + head worktree) at `identity-gate.sh:4966` and
    # `:4973`. Full tree 2,352 files / 94 MiB, pruned 2,199 / 25 MiB;
    # `git checkout-index -a` over each, three runs: 383/391/377ms against
    # 176/167/159ms, so ~217ms per checkout, and a whole full-path invocation
    # measured 5141/3866/3239ms before against 3047/2688/2728ms after.
    #
    # AND THE SAVING DOES **NOT** SCALE BY THE INVOCATION COUNT, which is the
    # cost model this comment carried while it was being written and which the
    # measurement disproved. Section 55 moved BOTH full checkouts behind the
    # fast path (see its banner at `identity-gate.sh:4934`, "most of them"
    # never get there), so multiplying ~217ms by two checkouts times every
    # invocation in the group is wrong: only the invocations that reach the
    # producer differential pay it at all. The group-level effect is real but
    # sits inside this suite's own run-to-run spread and is NOT demonstrable
    # from a single run -- the four post-change timings are recorded at `22f7`
    # alongside the four before them. Do not re-derive a per-invocation
    # multiplier from this comment.
    #
    # WHY A CLOSURE AND NOT A HAND-PICKED SET. Section 59's first attempt
    # seeded a `--no-checkout` clone with the code closure plus a few TODOs
    # and measured why that does not work: the kept TODOs' refs point at real
    # source files the minimal set does not contain, so both walks resolved
    # 0 of 134 refs instead of 47 and `corpus_resolution_snapshot` correctly
    # refused the vacuous baseline. The closure below is derived from the
    # RESOLVER'S OWN functions -- `section_candidate_files`, `classify_ref`
    # and `resolve_ref` -- never a backtick-regex approximation of the ref
    # grammar, so it cannot drift from the rule the gate actually applies.
    #
    # EMPTY PLACEHOLDERS, NOT DELETIONS, FOR THE REST OF THE .c/.h TREE.
    # `_basename_index` (ref_resolution.py:443) indexes every .c/.h path in
    # the tree, and a bare-basename ref resolves only when its basename is
    # UNIQUE there. Deleting 1,300 unrelated sources would turn ambiguous
    # basenames unique and silently re-resolve refs the full tree leaves in
    # `missing_file` -- measured on the first draft of this prune, which
    # moved five mappings (two `unpaired_ref` -> resolved, three
    # `no_calllike_token` -> `missing_file`). An empty file preserves the
    # index's shape exactly and costs no bytes; content is never read,
    # because every file any section scope names is in the closure.
    #
    # AND THE PRUNE PROVES ITSELF. A snapshot is taken over the FULL tree
    # before the prune and again after it, and the two `mappings` must be
    # byte-identical -- 134 refs, 47 resolved, same buckets. That check is
    # what makes the closure safe to re-derive on every run: a TODO edit that
    # reaches a file the derivation misses fails the fixture build loudly
    # instead of quietly changing the corpus the whole group compares against.
    python3 - "$PWD" "$TMP_DIR/gate-seed-cache.json" <<'PY'
import os, pathlib, subprocess, sys

root = pathlib.Path(sys.argv[1]).resolve()
cache = pathlib.Path(sys.argv[2]).resolve()

# TEST-ONLY: drop one leg of the closure derivation, so `22f8` can prove the
# before/after check below actually REFUSES an incomplete closure instead of
# being a comparison that can only ever agree with itself. Unset in every real
# seed; a value the derivation does not recognise is a hard error rather than a
# silent no-op, because a typo that quietly disabled the mutation would make the
# control pass while proving nothing.
DROP_LEG = os.environ.get("GATE_CLOSURE_DROP_LEG", "")
if DROP_LEG not in ("", "section_candidates", "classify", "resolve"):
    sys.stderr.write("gate_seed: unknown GATE_CLOSURE_DROP_LEG %r\n" % DROP_LEG)
    sys.exit(2)

# The cache lives OUTSIDE the fixture clone on purpose: the gate invocations
# build their own, and a stray build/todo-cache.json in the seeded tree would
# be committed into the fixture base and read by cases that never wrote it.
cache.parent.mkdir(parents=True, exist_ok=True)
rc = subprocess.run(
    [sys.executable, "scripts/todo-graph/build.py", "--root", "todo",
     "--output", str(cache), "--quiet"],
    cwd=str(root), capture_output=True, text=True)
if rc.returncode != 0:
    sys.stderr.write("gate_seed: could not build the fixture cache:\n"
                     + rc.stdout[-2000:] + rc.stderr[-2000:] + "\n")
    sys.exit(1)

os.environ["STUB_LINT_REPO_ROOT"] = str(root)
os.environ["STUB_LINT_CACHE"] = str(cache)
sys.path.insert(0, str(root / "scripts" / "todo-graph"))
import corpus_resolution_snapshot as crs   # noqa: E402
import ref_resolution as rr                # noqa: E402


def snapshot():
    with rr.walk_scope():
        return crs.collect()


def closure():
    """Every repo file the resolver consults for the kept corpus, via its own
    functions. `section_candidate_files` gives the authored pairing evidence
    (already basename-repaired to effective paths), `classify_ref` gives the
    file each symbol ref is finally checked against, and `resolve_ref` gives
    where it landed -- declaration-following can end in a header nothing
    authored.

    ALL THREE LEGS, AND ONLY ONE OF THEM FIRES THE CONTROL TODAY. Removing
    the `section_candidate_files` leg moves two mappings (`unpaired_ref` ->
    resolved: a symbol the full tree finds in several of its section's files
    and a pruned tree finds in one) and the before/after check refuses the
    prune, which is the control this derivation was verified against. The
    `classify_ref` leg is currently redundant for the six kept TODOs --
    removing it changes nothing -- because every basename-repaired symbol ref
    they carry names a file some `kind=file` ref names too. It is kept
    because that is a property of TODAY'S corpus, not of the rule: a symbol
    ref naming a bare basename no `kind=file` ref mentions needs its file
    present to stay in `no_calllike_token` instead of falling to
    `missing_file`, and the six kept TODOs are ordinary roadmap files that
    change under this fixture."""
    keep = set()

    def add(p):
        if not p:
            return
        try:
            rel = os.path.relpath(pathlib.Path(p).resolve(), root)
        except (OSError, ValueError):
            return
        if not rel.startswith(".."):
            keep.add(rel)

    nodes, _corpus = crs._load_nodes()
    with rr.walk_scope():
        for node in nodes:
            by_sec = {}
            for it in (node.get("stamped_items") or []):
                by_sec.setdefault(it.get("section_n"), []).append(it)
            for sec, items in by_sec.items():
                if DROP_LEG != "section_candidates":
                    for f in rr.section_candidate_files(items, root):
                        add(f)
                scope = rr.section_scope(items, root)
                for it in items:
                    for ref in (it.get("refs") or []):
                        if ref.get("kind") != "symbol":
                            continue
                        if DROP_LEG != "classify":
                            add(rr.classify_ref(ref, scope, root).abs_path)
                        if DROP_LEG != "resolve":
                            add(getattr(rr.resolve_ref(ref, scope, root),
                                        "def_rel", None))
    return keep


# BULK TREES ONLY. Everything outside these four carries the gate's own code,
# its lints, the corpus and the tooling the fixtures invoke by path, and is
# small (~500 files); pruning it would trade ~40ms of checkout for a fixture
# that breaks whenever a case reaches for a script it no longer has.
BULK = ("src", "include", "user", "resources")

before = snapshot()
keep = closure()

# NEVER WRITE THROUGH A LINK, AND NEVER WALK THROUGH ONE.
# `Path.is_file()` FOLLOWS a symlink and `write_bytes` opens the TARGET, so
# truncating a placeholder in place let a `.c`/`.h` symlink under a bulk tree
# zero out whatever it points at -- including an absolute path outside this
# clone, on a host running the tooling suite. Reproduced directly: a `link.c`
# pointing at an outside file reported `is_file() True`, and `write_bytes(b"")`
# emptied that file (Codex adversarial, section 59 round 2, [high]). No bulk
# tree carries a tracked symlink today, so this was latent -- but section 55
# deliberately kept `materialize_subtree` faithful to symlink entries rather
# than refusing them, so one appearing here is a state this repo has already
# decided to support. `22f9` is the regression control.
#
# The order below is the fix: `unlink()` removes the LINK and never the
# target, and the placeholder that replaces it is a fresh regular file at the
# same NAME, which is all `_basename_index` reads. `os.walk(followlinks=False)`
# replaces `rglob`, whose symlinked-directory behaviour is version-dependent
# (3.12 did not descend; relying on that is not a boundary).
pruned = placeholders = 0
for top in BULK:
    base = root / top
    if not base.is_dir() or base.is_symlink():
        continue
    for dirpath, dirnames, filenames in os.walk(base, topdown=False,
                                                followlinks=False):
        here = pathlib.Path(dirpath)
        for name in filenames:
            path = here / name
            rel = os.path.relpath(path, root)
            if rel in keep:
                continue
            path.unlink()
            if path.suffix in (".c", ".h"):
                path.write_bytes(b"")
                placeholders += 1
            else:
                pruned += 1
        # A symlinked DIRECTORY is listed in `dirnames`, never descended into,
        # and REMOVED AS A LINK WITH NO PLACEHOLDER -- it must not share the
        # branch above even when its name ends in `.c`. `_basename_index`
        # (`ref_resolution.py:443`) indexes `os.walk`'s FILENAMES only, so a
        # directory called `alias.c` is absent from the full tree's index;
        # replacing it with an empty regular file would ADD that basename to
        # the pruned index, and the before/after mapping check cannot see it
        # while no current ref names it -- a later fixture mutation would then
        # resolve differently here than in the full tree (Codex adversarial,
        # section 59 round 3, [medium]).
        for name in dirnames:
            sub = here / name
            if not sub.is_symlink():
                continue
            if os.path.relpath(sub, root) in keep:
                continue
            sub.unlink()
            pruned += 1
        # Drop the directories the prune emptied, so the fixture commit does
        # not carry a skeleton of `resources/` that nothing reads. `topdown`
        # is False, so a child is already gone by the time its parent is seen.
        for name in dirnames:
            sub = here / name
            if sub.is_symlink() or not sub.is_dir():
                continue
            if not any(sub.iterdir()):
                sub.rmdir()

after = snapshot()
if before != after:
    moved = sorted(k for k in set(before) | set(after)
                   if before.get(k) != after.get(k))
    sys.stderr.write(
        "gate_seed: pruning the fixture tree MOVED %d of %d resolutions -- the "
        "reference closure is incomplete, so the fixture would compare a "
        "corpus the live resolver does not produce:\n" % (len(moved), len(before)))
    for k in moved[:10]:
        sys.stderr.write("  %s\n    full: %r\n    pruned: %r\n"
                         % (k, before.get(k), after.get(k)))
    sys.exit(1)

resolved = sum(1 for v in before.values() if isinstance(v, list))
if not resolved:
    # Belt and braces against the vacuity the first attempt hit: an identical
    # BEFORE and AFTER is worth nothing if both resolved nothing.
    sys.stderr.write("gate_seed: the kept corpus resolves no refs at all, so "
                     "every comparison in this group would be vacuous\n")
    sys.exit(1)
sys.stderr.write("gate_seed: reference closure %d files, %d/%d refs resolved, "
                 "%d placeholders, %d files dropped\n"
                 % (len(keep), resolved, len(before), placeholders, pruned))
PY
}

gate_seed() {
    # Returns 0 if a usable fixture clone was built, 1 otherwise.
    rm -rf "$GATE_REPO" 2>/dev/null || true
    # HARDLINKED local clone. `--no-hardlinks` forced a physical copy of the
    # whole object store -- ~212 MiB in this checkout -- on every tooling run
    # (Codex perf, section 16). Hardlinks are safe here: git never rewrites an
    # existing object, the fixture only ADDS commits, and removing the clone
    # cannot affect the source through a hardlink.
    #
    # THE CLONE STAYS FULL; THE FIXTURE COMMIT DOES NOT. Section 59 first
    # tried `--no-checkout` here and measured why that does not work, kept so
    # the next reader does not re-spend the round trip: hand-seeding only the
    # closure array's files plus a handful of TODOs builds a repo the GATE can
    # run against (it reads only the closure it declares) but not one the
    # RESOLVER can WALK, so every walk resolved 0 of 134 refs instead of 47
    # and `corpus_resolution_snapshot` correctly refused the vacuous baseline.
    #
    # The cost that actually repeats is not this clone (once, ~0.5s) but the
    # fixture COMMIT'S TREE, which a FULL-PATH invocation checks out twice.
    # So the tree is pruned AFTER the seed is assembled, by
    # `gate_reference_closure_prune` below, against a closure derived from the
    # resolver's own functions and proved by a before/after snapshot that must
    # be byte-identical. That helper carries the measurements AND states why
    # the saving must not be multiplied by the group's invocation count (most
    # invocations never reach either checkout, `identity-gate.sh:4934`); do
    # not re-derive a cost model from this preamble.
    gate_seed_tree "$GATE_REPO" || return 1
    (
        cd "$GATE_REPO" || exit 1
        # Cut the tree that a full-path gate invocation checks out, down to
        # the closure the resolver itself consults. Fails the seed rather
        # than committing a corpus the live resolver does not produce. The
        # cost model (and what it is NOT) is at the helper's own comment.
        gate_reference_closure_prune || exit 1
        git add -A >/dev/null 2>&1
        git commit --quiet --no-verify -m "fixture base" >/dev/null 2>&1
    ) || return 1
    return 0
}

# The mutation: a PARTIAL loss, not a total one. resolve_ref() is the single
# shared verdict both consumers call, so forcing a bucket for odd-length symbol
# names drops some previously-resolved mappings while leaving others intact --
# exactly the shape a count-based check cannot see and this gate must.
gate_mutate() {
    python3 - "$GATE_REPO/scripts/todo-graph/ref_resolution.py" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
anchor = '    answer = _resolve_end_to_end(verdict.abs_path, ref.get("symbol"), repo_root)'
if anchor not in src:
    sys.exit(7)
# Tuple-guarded ON PURPOSE: only RESOLVED answers are bucketed. Overriding
# unconditionally also re-bucketed refs that were already unresolved, which
# shows up as CHANGED -- so the "gain" fixture (22g) could never isolate a pure
# GAINED, and its plain-compare leg failed for the wrong reason.
# NAMED AS AN ENUM MEMBER, not a raw string (section 20). The resolver's outcome
# constructors refuse any bucket outside the declared emitted set, so a raw
# `"no_calllike_token"` here now raises BucketContractError -- the mutation would
# fail as an error rather than as the partial verdict LOSS this fixture exists to
# stage, and the gate would be exercised on the wrong signal.
inject = (anchor + "\n"
          '    if not isinstance(answer, str) and len(ref.get("symbol") or "") % 2 == 1:\n'
          '        answer = Bucket.NO_CALLLIKE_TOKEN')
p.write_text(src.replace(anchor, inject, 1), encoding="utf-8")
PY
}

if ! gate_seed; then
    t_fail "identity gate: could not build the fixture clone"
else
    GATE_BASE="$(cd "$GATE_REPO" && git rev-parse HEAD)"

    # 22a: base == head is a clean no-op, not a spurious failure.
    if (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$GATE_BASE" --head HEAD \
            >"$TMP_DIR/gate-22a.log" 2>&1); then
        t_pass "identity gate: unchanged tree exits 0"
    else
        t_fail "identity gate: unchanged tree did not exit 0 (see $TMP_DIR/gate-22a.log)"
    fi

    # 22b: a TODO-ONLY edit must NOT trip the gate. This is the case that
    # sank the tracked-baseline design in section 12 -- occurrence keys embed
    # item_idx, so adding an item moved every later key and fired an ERROR on
    # ordinary roadmap work. Holding one cache across both walks is what makes
    # this pass, so if it ever fails the key-stability property is gone.
    (
        cd "$GATE_REPO" || exit 1
        t="$(find todo -name 'TODO-*.md' | sort | head -1)"
        printf '\n- [ ] a new item that shifts every later item_idx\n' >>"$t"
        # A BENIGN resolver edit rides along on purpose. Without it the closure
        # is byte-identical, the gate early-exits, and this case proves nothing
        # about key stability -- it would pass against a gate that never walks.
        # With it the double walk really runs over a corpus whose item_idx
        # values have shifted, which is the exact condition that sank section
        # 12's tracked-baseline design.
        printf '\n# identity-gate fixture: benign comment, no behaviour change\n' \
            >>scripts/todo-graph/ref_resolution.py
        git commit --quiet --no-verify -am "todo-only edit + benign resolver comment" >/dev/null 2>&1
    )
    if (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$GATE_BASE" --head HEAD \
            >"$TMP_DIR/gate-22b.log" 2>&1); then
        t_pass "identity gate: TODO-only edit does not trip it"
    else
        t_fail "identity gate: TODO-only edit tripped the gate (see $TMP_DIR/gate-22b.log)"
    fi
    GATE_TODO_SHA="$(cd "$GATE_REPO" && git rev-parse HEAD)"

    # 22c: THE FIRING CASE -- a resolver that loses mappings must FAIL with the
    # regression code (1), and the log must name the loss rather than merely
    # exiting non-zero.
    gate_mutate && (cd "$GATE_REPO" && git commit --quiet --no-verify -am "mutate resolver" >/dev/null 2>&1)
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$GATE_TODO_SHA" --head HEAD \
        >"$TMP_DIR/gate-22c.log" 2>&1)
    GATE_RC=$?
    if [ "$GATE_RC" -eq 1 ] && grep -q 'LOST' "$TMP_DIR/gate-22c.log"; then
        t_pass "identity gate: FIRES on a lost mapping (rc 1, names LOST)"
    else
        t_fail "identity gate: did not fire on a lost mapping (rc=$GATE_RC; see $TMP_DIR/gate-22c.log)"
    fi

    # 22d: mutation check for 22c -- reverting the defect must restore green.
    # Without this, 22c would also "pass" against a gate that fails always.
    (cd "$GATE_REPO" && git checkout --quiet HEAD~1 -- scripts/todo-graph/ref_resolution.py \
        && git commit --quiet --no-verify -am "revert mutation" >/dev/null 2>&1)
    if (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$GATE_TODO_SHA" --head HEAD \
            >"$TMP_DIR/gate-22d.log" 2>&1); then
        t_pass "identity gate: mutation-check -- reverting the defect restores green"
    else
        t_fail "identity gate: still red after reverting the defect (see $TMP_DIR/gate-22d.log)"
    fi

    # 22e: INFRASTRUCTURE FAILURE IS NOT A PASS. An unusable base must exit 3,
    # never 0 -- "the gate could not run" read as "the gate passed" is the
    # single failure mode that would make the whole wiring worthless.
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base 0000000000000000000000000000000000000000 \
        >"$TMP_DIR/gate-22e.log" 2>&1)
    GATE_RC=$?
    if [ "$GATE_RC" -eq 3 ]; then
        t_pass "identity gate: unusable base exits 3 (infrastructure, not pass)"
    else
        t_fail "identity gate: unusable base exited $GATE_RC, expected 3 (see $TMP_DIR/gate-22e.log)"
    fi

    # 22g: THE STRICT BRANCH ITSELF. 22c exercises the LOST path, which fails
    # with or without --strict, so every case above would stay green if
    # --strict were dropped from the driver or its `gained or added` branch
    # deleted -- the advertised protection against a resolver binding an
    # unresolved ref to the WRONG definition could regress silently (Codex
    # adversarial, section 16). This builds a real GAINED and asserts the two
    # verdicts DIFFER: plain compare 0, --strict 1 naming GAINED.
    #
    # The gain is manufactured the other way round from 22c's loss: take a
    # baseline from a resolver that refuses odd-length symbols, then compare
    # with the unmutated resolver, so refs that were bucketed now resolve.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet -- scripts/todo-graph/ref_resolution.py 2>/dev/null
        python3 scripts/todo-graph/build.py --quiet --output "$TMP_DIR/g-cache.json" >/dev/null 2>&1
    )
    gate_mutate
    (cd "$GATE_REPO" && STUB_LINT_CACHE="$TMP_DIR/g-cache.json" STUB_LINT_REPO_ROOT="$GATE_REPO" \
        python3 scripts/todo-graph/corpus_resolution_snapshot.py write "$TMP_DIR/g-base.json" \
        >"$TMP_DIR/gate-22g-write.log" 2>&1)
    (cd "$GATE_REPO" && git checkout --quiet -- scripts/todo-graph/ref_resolution.py)
    (cd "$GATE_REPO" && STUB_LINT_CACHE="$TMP_DIR/g-cache.json" STUB_LINT_REPO_ROOT="$GATE_REPO" \
        python3 scripts/todo-graph/corpus_resolution_snapshot.py compare "$TMP_DIR/g-base.json" \
        >"$TMP_DIR/gate-22g-plain.log" 2>&1)
    G_PLAIN=$?
    (cd "$GATE_REPO" && STUB_LINT_CACHE="$TMP_DIR/g-cache.json" STUB_LINT_REPO_ROOT="$GATE_REPO" \
        python3 scripts/todo-graph/corpus_resolution_snapshot.py compare "$TMP_DIR/g-base.json" --strict \
        >"$TMP_DIR/gate-22g-strict.log" 2>&1)
    G_STRICT=$?
    if ! grep -q 'GAINED' "$TMP_DIR/gate-22g-plain.log"; then
        t_fail "identity gate: fixture 22g produced no GAINED to test against"
    elif [ "$G_PLAIN" -eq 0 ] && [ "$G_STRICT" -eq 1 ] \
         && grep -q 'strict' "$TMP_DIR/gate-22g-strict.log"; then
        t_pass "identity gate: --strict rejects a GAINED that plain compare passes"
    else
        t_fail "identity gate: --strict did not flip a GAINED verdict (plain=$G_PLAIN strict=$G_STRICT; see $TMP_DIR/gate-22g-strict.log)"
    fi

    # 22h: the DRIVER must supply --strict. 22g proves the tool's strict branch
    # works when someone passes the flag; it says nothing about whether
    # identity-gate.sh does. Deleting `--strict` from the driver leaves 22g
    # green, and the only other driver fixture (22c) produces LOST, which fails
    # with or without it -- so the integration guard was absent (Codex
    # adversarial round 2, section 16). Here the GAIN is the whole diff: base
    # commit carries the mutated resolver, head restores it, so the driver's
    # own run must come back rc 1 naming GAINED.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet -- scripts/todo-graph/ref_resolution.py 2>/dev/null
    )
    gate_mutate
    (cd "$GATE_REPO" && git commit --quiet --no-verify -am "22h base: resolver that loses odd-length symbols" >/dev/null 2>&1)
    G_MUT_BASE="$(cd "$GATE_REPO" && git rev-parse HEAD)"
    (cd "$GATE_REPO" && git checkout --quiet HEAD~1 -- scripts/todo-graph/ref_resolution.py \
        && git commit --quiet --no-verify -am "22h head: restore the resolver (pure GAINED)" >/dev/null 2>&1)
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_MUT_BASE" --head HEAD \
        >"$TMP_DIR/gate-22h.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 1 ] && grep -q 'GAINED' "$TMP_DIR/gate-22h.log"; then
        t_pass "identity gate: the DRIVER passes --strict (a pure GAINED fails it)"
    else
        t_fail "identity gate: driver did not reject a pure GAINED (rc=$G_RC; see $TMP_DIR/gate-22h.log)"
    fi

    # 22j..22n: THE PROTOCOL CHECK MUST TRACK BUCKET VALUES, NOT THEIR SOURCE
    # TEXT, and -- since section 18 -- must SEPARATE a protocol migration from a
    # resolver change instead of refusing both alike.
    #
    # The constants moved to the inert snapshot_protocol.json in section 18, so
    # these fixtures mutate THAT rather than ref_resolution.py's source text.
    # The section-16 contract they replaced ("any bucket change fails closed")
    # is deliberately no longer asserted: failing closed was the SYMPTOM of the
    # constants living beside the logic, and 22l keeps the fail-closed
    # direction for the case that genuinely cannot be separated.
    (cd "$GATE_REPO" && git checkout --quiet -- scripts/todo-graph/ref_resolution.py 2>/dev/null)
    G_PROTO_BASE="$(cd "$GATE_REPO" && git rev-parse HEAD)"

    # 22j: a bucket the corpus never exercises, added ALONE, is provably
    # verdict-neutral -- every executable closure file is byte-identical, so
    # the differential runs and finds nothing. Under section 16 this was rc 3.
    (
        cd "$GATE_REPO" || exit 1
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
assert "post_resolution_buckets" in d, "protocol file shape changed"
d["post_resolution_buckets"].append("never_used_by_any_ref")
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22j: add an unused bucket (data only)" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22j.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 0 ] && grep -q 'DATA-ONLY' "$TMP_DIR/gate-22j.log"; then
        t_pass "identity gate: a data-only bucket addition runs the differential and passes"
    else
        t_fail "identity gate: data-only bucket addition not separated (rc=$G_RC; see $TMP_DIR/gate-22j.log)"
    fi

    # 22l: THE FAIL-CLOSED DIRECTION, which section 18 must NOT weaken. A
    # protocol change bundled with an executable-closure change cannot be
    # separated, so the gate still refuses -- and names splitting the commit.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["post_resolution_buckets"].append("bundled_with_a_resolver_change")
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        printf '\n# 22l: a resolver edit riding along with the protocol change\n' \
            >> scripts/todo-graph/ref_resolution.py
        git commit --quiet --no-verify -am "22l: protocol + resolver together" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22l.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] && grep -q 'Split the commit' "$TMP_DIR/gate-22l.log"; then
        t_pass "identity gate: a protocol change bundled with a resolver change still fails closed"
    else
        t_fail "identity gate: bundled protocol+resolver change was not refused (rc=$G_RC; see $TMP_DIR/gate-22l.log)"
    fi

    # 22m: a SCHEMA change alone. The differential is mechanically impossible
    # across formats, so the gate passes on the byte-identical-executables
    # inference -- and must SAY so, not pass silently.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/ref_resolution.py
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["snapshot_schema"] = int(d["snapshot_schema"]) + 1
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22m: schema bump, data only" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22m.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 0 ] && grep -q 'schema migrated' "$TMP_DIR/gate-22m.log"; then
        t_pass "identity gate: a data-only schema migration passes on the byte-identical-executables proof"
    else
        t_fail "identity gate: data-only schema migration not handled (rc=$G_RC; see $TMP_DIR/gate-22m.log)"
    fi

    # 22k: a comment in the RESOLVER is not a protocol change. This is the
    # false-positive direction -- without it, a fix for 22j that hashed more
    # source text would pass 22j and silently wedge the gate.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/ref_resolution.py
        printf '\n# 22k: an adjacent comment, no behaviour change\n' \
            >> scripts/todo-graph/ref_resolution.py
        git commit --quiet --no-verify -am "22k: comment in the resolver" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22k.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 0 ]; then
        t_pass "identity gate: a comment in the resolver is not a protocol change"
    else
        t_fail "identity gate: adjacent comment wedged the gate (rc=$G_RC; see $TMP_DIR/gate-22k.log)"
    fi

    # 22n: THE PRODUCER BLIND SPOT, which is the whole reason section 18
    # exists. A build.py that stops emitting a stamped ref removes it from BOTH
    # walks, so the resolver differential sees two agreeing sides and passes.
    # The producer differential must fail -- and note the mutation drops the
    # refs unconditionally, so the head corpus carries no live example either,
    # which is exactly the case a head-corpus-only comparison would miss.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/ref_resolution.py \
            scripts/todo-graph/snapshot_protocol.json
        python3 - scripts/todo-graph/build.py <<'PY2'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); s = p.read_text(encoding="utf-8")
old = '    nodes.sort(key=lambda n: n["file_path"])'
assert old in s, "producer publish anchor not found"
# A REGRESSION SHAPED LIKE A REAL ONE: silently drop every symbol ref from the
# per-item population the resolver walk consumes.
inject = [
    old,
    '    for _n in nodes:',
    '        for _it in _n.get("stamped_items") or []:',
    '            _it["refs"] = [_r for _r in (_it.get("refs") or [])',
    '                           if _r.get("kind") != "symbol"]',
]
p.write_text(s.replace(old, "\n".join(inject), 1), encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22n: producer drops every symbol ref" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22n.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 1 ] && grep -q 'DROPPED' "$TMP_DIR/gate-22n.log"; then
        t_pass "identity gate: the producer differential catches a dropped stamped ref"
    else
        t_fail "identity gate: producer regression not caught (rc=$G_RC; see $TMP_DIR/gate-22n.log)"
    fi

    # 22o: BOTH the schema AND the buckets changed. The schema fast path exits
    # 0 WITHOUT walking, and a bucket rename is a real verdict change only the
    # differential can surface -- but the differential is impossible across
    # schemas. Neither half of the answer exists, so it must fail closed. The
    # first cut let the schema branch swallow the bundled vocabulary change
    # (Codex adversarial, section 18).
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/ref_resolution.py scripts/todo-graph/build.py
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["snapshot_schema"] = int(d["snapshot_schema"]) + 1
d["post_resolution_buckets"].append("bundled_with_a_schema_bump")
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22o: schema AND buckets together" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22o.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] && grep -q 'BOTH the snapshot schema' "$TMP_DIR/gate-22o.log"; then
        t_pass "identity gate: a schema bump bundled with a bucket change fails closed"
    else
        t_fail "identity gate: bundled schema+bucket change was not refused (rc=$G_RC; see $TMP_DIR/gate-22o.log)"
    fi

    # 22p: A CROSS-BOUNDARY MOVE. `ALL_BUCKETS` is PRE + POST, so moving the
    # first POST bucket to the end of PRE leaves the concatenation
    # byte-identical -- while changing behaviour, because the consumer branches
    # on POST membership to decide whether to report the effective path or the
    # authored one. Fingerprinting the flattened tuple could not see it.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
moved = d["post_resolution_buckets"].pop(0)
d["pre_resolution_buckets"].append(moved)
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22p: move a bucket across the pre/post boundary" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22p.log" 2>&1)
    G_RC=$?
    # The concatenation is unchanged and every bucket STRING is unchanged, so
    # the mapping diff reports nothing -- the relocation is only visible in the
    # two halves the snapshots now record separately. It must FAIL, not merely
    # run: asserting "the DATA-ONLY branch ran" was the round-1 defect, because
    # the move then shipped green after bypassing the schema fast path.
    if [ "$G_RC" -eq 1 ] && grep -q 'RELOCATED' "$TMP_DIR/gate-22p.log"; then
        t_pass "identity gate: a pre/post boundary move FAILS even though the flattened tuple is identical"
    else
        t_fail "identity gate: cross-boundary bucket move was not failed (rc=$G_RC; see $TMP_DIR/gate-22p.log)"
    fi

    # 22q: a schema value the runtime loader REJECTS must not reach the fast
    # path. proto_of accepted any non-boolean int while snapshot_protocol.load
    # requires a positive one, so a data-only 2 -> 0 exited 0 without ever
    # importing the loader that would refuse the tree.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["snapshot_schema"] = 0
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22q: schema 0, which the loader refuses" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22q.log" 2>&1)
    G_RC=$?
    # THE STABLE TOKEN, not only the shared prose (Codex test-coverage,
    # section 50). Both sides emit "cannot read the snapshot protocol", so a
    # prose-only assertion would still pass if the gate died on the BASE side
    # before ever reaching the head-side defect this case is about.
    if [ "$G_RC" -eq 3 ] && grep -q 'HEAD_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22q.log" \
       && grep -q 'cannot read the snapshot protocol' "$TMP_DIR/gate-22q.log" \
       && ! grep -q 'BASE_PROTOCOL_' "$TMP_DIR/gate-22q.log"; then
        t_pass "identity gate: a schema the loader would reject is infrastructure, not a fast-path PASS"
    else
        t_fail "identity gate: unusable schema reached a verdict (rc=$G_RC; see $TMP_DIR/gate-22q.log)"
    fi

    # 22r: OBJECT-VALUED HALVES whose ordered keys reproduce the old digest.
    # proto_of normalised with list(...) BEFORE validating, so a JSON object
    # hashed identically to the real list while snapshot_protocol.load rejected
    # it -- bundled with a schema bump it left BUCKETS_CHANGED false and the
    # fast path exited 0 for a protocol nothing can load.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["post_resolution_buckets"] = {n: i for i, n in
                                enumerate(d["post_resolution_buckets"])}
d["snapshot_schema"] = int(d["snapshot_schema"]) + 1
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22r: object-valued halves + schema bump" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22r.log" 2>&1)
    G_RC=$?
    # THE STABLE TOKEN, not only the shared prose (Codex test-coverage,
    # section 50). Both sides emit "cannot read the snapshot protocol", so a
    # prose-only assertion would still pass if the gate died on the BASE side
    # before ever reaching the head-side defect this case is about.
    if [ "$G_RC" -eq 3 ] && grep -q 'HEAD_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22r.log" \
       && grep -q 'cannot read the snapshot protocol' "$TMP_DIR/gate-22r.log" \
       && ! grep -q 'BASE_PROTOCOL_' "$TMP_DIR/gate-22r.log"; then
        t_pass "identity gate: object-valued halves are unreadable, not a digest match"
    else
        t_fail "identity gate: object-valued halves passed (rc=$G_RC; see $TMP_DIR/gate-22r.log)"
    fi

    # 22s: a RENAME PAIRED WITH A MOVE. The old name reads as a removal and the
    # new one as an addition, so a name-by-name comparison sees no relocation
    # at all -- and an unused bucket produces no mapping difference either, so
    # the pair shipped green at rc 0.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
# Remove a PRE bucket and add a differently-named one to POST.
d["pre_resolution_buckets"].remove("path_escape")
d["post_resolution_buckets"].append("path_escaped")
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22s: rename paired with a cross-half move" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22s.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 1 ] && grep -qE 'RELOCATED|undeclared_removals' "$TMP_DIR/gate-22s.log"; then
        t_pass "identity gate: an undeclared bucket removal is refused (ambiguous with a rename)"
    else
        t_fail "identity gate: rename+move escaped (rc=$G_RC; see $TMP_DIR/gate-22s.log)"
    fi

    # 22t: THE CLEARING PATH ITSELF. 22s proves an undeclared removal is
    # refused; this proves the declaration actually clears it, because a
    # refusal with no working remedy is a wedge, not a gate. Declaring the
    # rename ALSO makes the cross-half move visible as the move it is.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["pre_resolution_buckets"].remove("path_escape")
d["pre_resolution_buckets"].append("path_escaped")
d["renamed_buckets"] = {"path_escape": "path_escaped"}
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22t: a DECLARED rename, same half" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22t.log" 2>&1)
    G_RC=$?
    # A DATA-ONLY RENAME IS REFUSED, and this fixture asserted the opposite
    # until the final adversarial round. Bucket names are hardcoded in
    # ref_resolution.py's emitters and in check_stub_behind_stamp.py's coverage
    # keys, so renaming the vocabulary alone leaves both producing and
    # expecting the retired name -- invisible while the bucket is dormant, and
    # invalid for every snapshot the day a ref lands in it. The fixture proved
    # only that its own pruned corpus did not exercise the bucket.
    if [ "$G_RC" -ne 0 ] && grep -q 'cannot be a data-only migration' "$TMP_DIR/gate-22t.log"; then
        t_pass "identity gate: a data-only rename is REFUSED (bucket names are hardcoded in the emitters)"
    else
        t_fail "identity gate: data-only rename was accepted (rc=$G_RC; see $TMP_DIR/gate-22t.log)"
    fi

    # 22u: a declared rename that ALSO crosses the boundary is a MOVE, and must
    # fail as one -- the declaration is what makes it visible.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["pre_resolution_buckets"].remove("path_escape")
d["post_resolution_buckets"].append("path_escaped")
d["renamed_buckets"] = {"path_escape": "path_escaped"}
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22u: declared rename that also moves half" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22u.log" 2>&1)
    G_RC=$?
    # Refused as a rename before the move is even reached -- strictly stronger
    # than reporting it as a relocation, and for the same underlying reason.
    if [ "$G_RC" -eq 1 ] && grep -q 'cannot be a data-only migration' "$TMP_DIR/gate-22u.log"; then
        t_pass "identity gate: a declared rename that crosses the boundary is refused"
    else
        t_fail "identity gate: declared rename+move was accepted (rc=$G_RC; see $TMP_DIR/gate-22u.log)"
    fi

    # 22v: A RENAME DECLARATION THAT COLLIDES with an existing bucket. Applying
    # declarations unvalidated collapsed two base entries onto one target and
    # silently erased a genuine same-name move; a declaration written by the
    # very commit under judgement must be validated against the real delta.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
# Move a bucket across the boundary AND declare a rename onto a name that
# already exists, which is what made the collapse possible.
d["pre_resolution_buckets"].remove("path_escape")
d["post_resolution_buckets"].append("path_escape")
d["renamed_buckets"] = {"missing_file": "no_calllike_token"}
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22v: colliding rename declaration" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22v.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -ne 0 ] && grep -q 'bad_declarations' "$TMP_DIR/gate-22v.log"; then
        t_pass "identity gate: a colliding rename declaration is rejected, not applied"
    else
        t_fail "identity gate: colliding rename declaration was applied (rc=$G_RC; see $TMP_DIR/gate-22v.log)"
    fi

    # 22w: a STALE retirement -- naming a bucket that was never in base -- is a
    # self-authored exemption left lying around for a future migration.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["retired_buckets"] = ["a_bucket_that_never_existed"]
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22w: stale retirement declaration" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22w.log" 2>&1)
    G_RC=$?
    # A retirement naming a bucket absent from BOTH sides is INERT, and the
    # gate deliberately accepts it. The two requirements here are in direct
    # tension and only one is implementable: rejecting it (so a pre-authorised
    # removal cannot sit waiting) requires distinguishing "never existed" from
    # "retired in an earlier commit", and the gate sees only base..head, not
    # history. Rejecting it therefore also rejects the declaration that a
    # LEGITIMATE migration must leave behind, which wedges every later commit
    # (fixture 22x). Edge-bound is the honest rule; the residual -- a
    # pre-declared retirement pre-authorises one future removal -- is recorded
    # as an accepted limitation on the section rather than papered over.
    if [ "$G_RC" -eq 0 ]; then
        t_pass "identity gate: an inert retirement declaration does not fail an unrelated commit"
    else
        t_fail "identity gate: inert retirement wedged an unrelated commit (rc=$G_RC; see $TMP_DIR/gate-22w.log)"
    fi

    (cd "$GATE_REPO" && git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
        && git commit --quiet --no-verify -am "restore after round-3 fixtures" >/dev/null 2>&1)
    # 22x: THE DECLARATION LIFECYCLE, over TWO commits. A migration must carry
    # its declaration, and that declaration then stays in the file -- so on the
    # NEXT commit the renamed-from name is absent from both sides. Treating
    # that as stale rejected every later verdict-safe commit until someone made
    # a cleanup commit no lifecycle described. Declarations are edge-bound.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/ref_resolution.py
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
i = d["pre_resolution_buckets"].index("path_escape")
d["pre_resolution_buckets"][i] = "path_escaped"
d["renamed_buckets"] = {"path_escape": "path_escaped"}
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        # MIGRATE THE EMITTER IN THE SAME COMMIT (section 20). A rename applied
        # to the vocabulary alone now leaves `EMITTED_BUCKETS` naming a member
        # the enum no longer has, so the resolver cannot even import and every
        # later commit fails as infrastructure. That is the contract working --
        # a data-only rename is exactly what this gate refuses -- but it makes
        # the tree this fixture leaves behind incoherent, and the property under
        # test is about a COMPLETED migration, not a half-done one.
        python3 - scripts/todo-graph/ref_resolution.py <<'PY2'
import pathlib, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text(encoding="utf-8")
if "Bucket.PATH_ESCAPE," not in s:
    sys.stderr.write("fixture 22x: the emitted-set member is not where expected\n")
    raise SystemExit(7)
p.write_text(s.replace("Bucket.PATH_ESCAPE", "Bucket.PATH_ESCAPED"),
             encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22x-1: declared rename migration" >/dev/null 2>&1
    )
    G_MIG_SHA="$(cd "$GATE_REPO" && git rev-parse HEAD)"
    # Second commit: touches only executable code, protocol untouched, and the
    # completed declaration is still sitting in the file.
    (
        cd "$GATE_REPO" || exit 1
        printf '\n# 22x-2: an ordinary resolver comment, no protocol change\n' \
            >> scripts/todo-graph/ref_resolution.py
        git commit --quiet --no-verify -am "22x-2: executable-only change after the migration" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_MIG_SHA" --head HEAD \
        >"$TMP_DIR/gate-22x.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 0 ]; then
        t_pass "identity gate: a completed declaration does not brick the next executable-only commit"
    else
        t_fail "identity gate: retained declaration wedged an unrelated commit (rc=$G_RC; see $TMP_DIR/gate-22x.log)"
    fi
    (cd "$GATE_REPO" && git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
        scripts/todo-graph/ref_resolution.py \
        && git commit --quiet --no-verify -am "restore after lifecycle fixture" >/dev/null 2>&1)

    # 22ad: A DORMANT RETIREMENT still leaves the emitters able to produce the
    # retired name. `path_escape` is hardcoded in ref_resolution.py and in
    # check_stub_behind_stamp.py coverage keys, so removing it from the
    # vocabulary while both still emit it is invalid for a reachable input --
    # dormant only until a ref meets that condition. Same latent-failure
    # argument that rejects renames, applied to retirements.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["pre_resolution_buckets"].remove("path_escape")
d["retired_buckets"] = ["path_escape"]
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22ad: retire a bucket the emitters still hardcode" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22ad.log" 2>&1)
    G_RC=$?
    # The MESSAGE moved with the evidence (section 20): the refusal now names
    # the declaration rather than a source-text hit. Asserting on it is the
    # point -- an assertion on rc alone is satisfied by any failure that exits
    # non-zero, including one that never reached the retirement branch.
    if [ "$G_RC" -ne 0 ] && grep -q 'still declares it in EMITTED_BUCKETS' "$TMP_DIR/gate-22ad.log"; then
        t_pass "identity gate: retiring a bucket the resolver still declares is refused"
    else
        t_fail "identity gate: dormant retirement accepted (rc=$G_RC; see $TMP_DIR/gate-22ad.log)"
    fi

    # 22ae: THE CLEARING PATH. Once the emitters no longer name it, the
    # data-only retirement is safe and must be allowed -- otherwise the rule is
    # a wedge rather than a gate.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["post_resolution_buckets"].append("never_emitted_anywhere")
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22ae-1: introduce a bucket no emitter names" >/dev/null 2>&1
    )
    G_RETIRE_BASE="$(cd "$GATE_REPO" && git rev-parse HEAD)"
    (
        cd "$GATE_REPO" || exit 1
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["post_resolution_buckets"].remove("never_emitted_anywhere")
d["retired_buckets"] = ["never_emitted_anywhere"]
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22ae-2: retire it, no emitter names it" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_RETIRE_BASE" --head HEAD \
        >"$TMP_DIR/gate-22ae.log" 2>&1)
    G_RC=$?
    # APPROVED, and this is the clearing path OPENING (section 20). It asserted
    # approval originally, was flipped to expect refusal at section 18's ship
    # gate because source-text absence was not proof of non-emission, and flips
    # back now that the resolver DECLARES its emitted set and refuses anything
    # outside it at runtime. `never_emitted_anywhere` is not in EMITTED_BUCKETS,
    # so the resolver provably cannot produce it and the data-only retirement
    # is safe. If this ever refuses again, the rule has become a wedge.
    if [ "$G_RC" -eq 0 ]; then
        t_pass "identity gate: retiring a bucket the resolver does not declare is approved"
    else
        t_fail "identity gate: the clearing path is closed -- a provably unemitted bucket could not be retired (rc=$G_RC; see $TMP_DIR/gate-22ae.log)"
    fi
    (cd "$GATE_REPO" && git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
        && git commit --quiet --no-verify -am "restore after retirement fixtures" >/dev/null 2>&1)

    # 22af: AN UNPARSEABLE EMITTER IS NOT EVIDENCE OF ABSENCE. The retirement
    # proof used to ask "does this bucket name appear in the emitters"; the
    # first cut returned a sentinel string on a read failure, which matches no
    # bucket name, so an unreadable emitter APPROVED the retirement -- the guard
    # was inverted, exactly the fail-open shape it was written to prevent.
    #
    # SECTION 20 CHANGED THE EVIDENCE, SO THIS FIXTURE FOLLOWS IT TO THE NEW
    # ONE. The proof is now the resolver's DECLARED emitted set, so the file
    # that must not be silently unreadable is `ref_resolution.py` itself: a
    # checker that cannot parse it knows nothing about what it emits, and an
    # empty answer would approve every retirement at once. The refusal is
    # INFRASTRUCTURE (rc 3), which is stronger than the old rc 1 -- "the gate
    # could not run" is a different claim from "the migration is invalid".
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["pre_resolution_buckets"].remove("path_escape")
d["retired_buckets"] = ["path_escape"]
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        printf '\ndef 22af_unparseable(:\n' >> scripts/todo-graph/ref_resolution.py
        git commit --quiet --no-verify -am "22af: retire a bucket with an unparseable emitter" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22af.log" 2>&1)
    G_RC=$?
    # SPECIFIC CODE AND DIAGNOSTIC. Accepting any non-zero meant a gate that
    # failed for an unrelated reason -- or failed always -- satisfied this
    # assertion without ever exercising the unreadable-emitter path.
    if [ "$G_RC" -eq 3 ] && grep -q 'NOT evidence that any bucket is unemitted' "$TMP_DIR/gate-22af.log"; then
        t_pass "identity gate: an unparseable emitter is not evidence a bucket is retired-safe"
    else
        t_fail "identity gate: unparseable-emitter path not exercised (rc=$G_RC; see $TMP_DIR/gate-22af.log)"
    fi
    (cd "$GATE_REPO" && git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
        scripts/todo-graph/ref_resolution.py \
        && git commit --quiet --no-verify -am "restore after unparseable-emitter fixture" >/dev/null 2>&1)

    # 22ag: LOADER/DATA DIVERGENCE. Reading only the JSON let the loader and
    # the data disagree: filtering a DORMANT bucket in the loader while the
    # JSON still declares it leaves both snapshots verdict-identical, so the
    # differential runs and passes -- and runtime consumers then no longer
    # recognise a bucket the published protocol declares.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/snapshot_protocol.py
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["post_resolution_buckets"].append("declared_but_filtered_out")
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        python3 - scripts/todo-graph/snapshot_protocol.py <<'PY2'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); s = p.read_text(encoding="utf-8")
old = "    return schema, pre, post"
assert old in s, "loader return anchor not found"
new = ('    post = tuple(b for b in post if b != "declared_but_filtered_out")\n'
       "    return schema, pre, post")
p.write_text(s.replace(old, new, 1), encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22ag: loader filters a bucket the JSON declares" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22ag.log" 2>&1)
    G_RC=$?
    # Loader/data divergence makes the protocol UNREADABLE, which the caller
    # treats as infrastructure -- assert that exact code and diagnostic rather
    # than "something went wrong".
    # THE STABLE TOKEN, not only the shared prose (Codex test-coverage,
    # section 50). Both sides emit "cannot read the snapshot protocol", so a
    # prose-only assertion would still pass if the gate died on the BASE side
    # before ever reaching the head-side defect this case is about.
    if [ "$G_RC" -eq 3 ] && grep -q 'HEAD_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22ag.log" \
       && grep -q 'cannot read the snapshot protocol' "$TMP_DIR/gate-22ag.log" \
       && ! grep -q 'BASE_PROTOCOL_' "$TMP_DIR/gate-22ag.log"; then
        t_pass "identity gate: a loader that disagrees with the protocol data is refused"
    else
        t_fail "identity gate: loader/data divergence not exercised (rc=$G_RC; see $TMP_DIR/gate-22ag.log)"
    fi
    (cd "$GATE_REPO" && git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
        scripts/todo-graph/snapshot_protocol.py \
        && git commit --quiet --no-verify -am "restore after loader-divergence fixture" >/dev/null 2>&1)

    # 22am / 22an: A MOVED PRODUCER CONTRACT IDENTITY MUST NOT WEDGE THE GATE.
    # The shared cache is built by the HEAD producer, and cache_schema refuses
    # any cache whose binding declares an identity other than the reader's own
    # -- with no profile opt-out, by design. So the BASE reader could not read
    # the one cache the gate builds, and the gate died rc 3 without walking
    # anything. NOT hypothetical: ebb3faf6e moved the digest and todo-graph.yml
    # failed on every push after it (run 31287999668, 2026-08-09), and it could
    # not self-clear -- the last-gated SHA only advances on a PASS, so the base
    # stayed pinned before the change.
    #
    # Bumping CACHE_FORMAT_VERSION is the smallest honest way to stage it: it is
    # the documented hand-bump for a semantic change, it moves the digest (the
    # version is inside the payload), and it touches no emission, so any verdict
    # difference would be the gate's own doing.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/cache_schema.py
        python3 - scripts/todo-graph/cache_schema.py <<'PY2'
import re, sys, pathlib
# BUMP WHATEVER IS THERE, never a hardcoded 1 -> 2. The clone is seeded from the
# WORKING TREE (see the closure copy above), so this file carries the live
# constant -- and the day section 27 legitimately bumped it to 2, a literal
# "CACHE_FORMAT_VERSION = 1" needle stopped matching, the fixture committed no
# change, base and head became identical, and BOTH gate cases failed reporting
# "not handled" as though the gate had regressed.
p = pathlib.Path(sys.argv[1]); s = p.read_text(encoding="utf-8")
m = re.search(r"^CACHE_FORMAT_VERSION = (\d+)$", s, re.M)
assert m, "cache format version anchor not found"
p.write_text(s[:m.start()] + f"CACHE_FORMAT_VERSION = {int(m.group(1)) + 1}"
             + s[m.end():], encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22am: hand-bump the producer contract version" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22am.log" 2>&1)
    G_RC=$?
    # ASSERT THE WALK HAPPENED, not merely rc 0. A gate that exited 0 down some
    # early-exit path would satisfy the code alone while adjudicating nothing --
    # which is the precise failure shape this whole fixture family exists for.
    if [ "$G_RC" -eq 0 ] && grep -q 'each walk reads a cache built by its own producer' "$TMP_DIR/gate-22am.log" \
       && grep -q 'both walks finished' "$TMP_DIR/gate-22am.log"; then
        t_pass "identity gate: a moved producer contract identity still walks both sides"
    else
        t_fail "identity gate: contract-identity change not handled (rc=$G_RC; see $TMP_DIR/gate-22am.log)"
    fi

    # 22an: THE MUTATION CHECK. Disarm the divergence branch in the clone's own
    # driver and the SAME fixture must go back to the rc 3 wedge -- otherwise
    # 22am would pass for some unrelated reason and prove nothing about the fix.
    # THE MUTANT IS A SEPARATE FILE, as every other mutation case here already
    # does it. This one edited the canonical driver IN PLACE, which section 54
    # turned into a refusal in its own right: the gate now checks that the
    # running `scripts/todo-graph/identity-gate.sh` matches the copy in the head
    # commit, so an in-place mutation produces GATE_DRIVER_NOT_AT_HEAD and this
    # case would assert a message it can no longer reach. A copy at another path
    # is invisible to that check by design -- it is the canonical path the hook
    # executes, not "any script", that has to match its own commit.
    G_MUT_AN="$GATE_REPO/scripts/todo-graph/identity-gate-nodiverge.sh"
    python3 - "$GATE_IN_CLONE" "$G_MUT_AN" <<'PY2'
import sys, pathlib
src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
s = src.read_text(encoding="utf-8")
old = "    CONTRACT_DIVERGED=1\n"
assert old in s, "divergence assignment anchor not found"
dst.write_text(s.replace(old, "    CONTRACT_DIVERGED=0\n", 1), encoding="utf-8")
PY2
    (cd "$GATE_REPO" && bash "$G_MUT_AN" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22an.log" 2>&1)
    G_RC=$?
    # THE MESSAGE IS THE VERSION HALF, and that is deliberate rather than a
    # looser match: `_check_identity` compares the version BEFORE the digest, so
    # a hand-bump trips the first branch. CI hit the second one (a digest moved
    # at an unchanged version) -- same check, same refusal, same wedge. Asserting
    # the exact message this mutation produces keeps the fixture honest; if the
    # ordering ever changes, this fails loudly instead of matching anything.
    if [ "$G_RC" -eq 3 ] && grep -q 'this reader speaks' "$TMP_DIR/gate-22an.log"; then
        t_pass "identity gate: mutation-check -- disarming the divergence branch restores the rc 3 wedge"
    else
        t_fail "identity gate: divergence branch is not load-bearing (rc=$G_RC; see $TMP_DIR/gate-22an.log)"
    fi
    rm -f "$G_MUT_AN"
    (cd "$GATE_REPO" && git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/cache_schema.py \
        && git commit --quiet --no-verify -am "restore after contract-identity fixture" >/dev/null 2>&1)

    # 22ah / 22ai: THE TWO SHAPES THAT DEFEATED THE SOURCE-TEXT PROOF (section
    # 20). Both preserve behaviour, both leave the quoted bucket literal absent,
    # and both therefore PASSED section 18's retirement guard while the resolver
    # could still emit the name. The declared-emission contract must reject them
    # as retirement candidates. Run against a COPY of the emitter so a fixture
    # cannot damage the tree under test.
    _bucket_mutation() {   # $1=label  $2=python-edit  $3=expected-message
        local _mut="$TMP_DIR/emit-mut.py"
        python3 - "$GATE_REPO/scripts/todo-graph/ref_resolution.py" "$_mut" "$2" <<'PY2'
import pathlib, sys
src, dst, edit = sys.argv[1], sys.argv[2], sys.argv[3]
t = pathlib.Path(src).read_text(encoding="utf-8")
old, new = edit.split("=>", 1)
if old not in t:
    sys.stderr.write("fixture cannot apply its own mutation: %r absent\n" % old)
    raise SystemExit(2)
t = t.replace(old, new, 1)
# Retire the member from the declaration too: the point is an emission the
# declared set no longer covers, which is exactly the two-commit sequence.
t = t.replace("    Bucket.PATH_ESCAPE,\n", "", 1)
pathlib.Path(dst).write_text(t, encoding="utf-8")
PY2
        if [ $? -ne 0 ]; then
            t_fail "bucket-emission contract: fixture setup failed for $1"
            return
        fi
        python3 "$GATE_REPO/scripts/lint/check_bucket_emission.py" \
            --emitter "$_mut" \
            --protocol "$GATE_REPO/scripts/todo-graph/snapshot_protocol.json" \
            >"$TMP_DIR/emit-mut.out" 2>"$TMP_DIR/emit-mut.err"
        local _rc=$?
        # ASSERT THE MESSAGE, NOT ONLY THE CODE. rc 1 is reachable from any
        # violation, including one produced by a broken fixture that never
        # exercised the shape under test.
        if [ "$_rc" -eq 1 ] && grep -q "$3" "$TMP_DIR/emit-mut.err"; then
            t_pass "bucket-emission contract: $1 is rejected as a retirement candidate"
        else
            t_fail "bucket-emission contract: $1 accepted (rc=$_rc; see $TMP_DIR/emit-mut.err)"
        fi
    }
    _bucket_mutation "indexed emission" \
        "return (None, Bucket.PATH_ESCAPE)=>return (None, PRE_RESOLUTION_BUCKETS[1])" \
        "is indexed"
    _bucket_mutation "concatenated emission" \
        "return (None, Bucket.PATH_ESCAPE)=>return (None, \"path\" + \"_escape\")" \
        "appears as a string"
    # A MEMBER DROPPED FROM THE DECLARATION WHILE ITS EMISSION SITES REMAIN.
    # This produced ZERO violations until round 2: the retirement gate reads
    # only the declaration, so the following data-only retirement was approved
    # while four live sites still named the bucket. The 22ae clearing-path
    # fixture cannot catch it -- it retires a bucket nothing ever emitted, so no
    # site is left behind to go stale. `_bucket_mutation` already removes the
    # member, so the no-op edit below leaves the sites in place.
    _bucket_mutation "declaration dropped, emission site left behind" \
        "return (None, Bucket.PATH_ESCAPE)=>return (None, Bucket.PATH_ESCAPE)" \
        "is named here but is NOT in"
    # THE THREE NON-CANONICAL ENUM ACCESSES. Matching only `Bucket.MEMBER` left
    # all three invisible (post-commit adversarial round). The iteration form is
    # the dangerous one: `Bucket` is derived from the JSON, so retiring the
    # first member makes the SAME expression return the next still-declared one
    # -- the branch keeps emitting, silently reclassified, and `_check_emitted`
    # approves it because the value IS declared.
    _bucket_mutation "nested-attribute enum access" \
        "return (None, Bucket.UNSUPPORTED_LANG)=>return (None, _protocol.Bucket.UNSUPPORTED_LANG)" \
        "reached through"
    _bucket_mutation "enum iteration" \
        "return (None, Bucket.UNSUPPORTED_LANG)=>return (None, next(iter(Bucket)))" \
        "bare .Bucket. is used"
    _bucket_mutation "enum alias" \
        "return (None, Bucket.UNSUPPORTED_LANG)=>return (None, (lambda B: B.UNSUPPORTED_LANG)(Bucket))" \
        "bare .Bucket. is used"
    # A RENAME MUST DISARM NOTHING SILENTLY. The bans are string copies of
    # resolver symbols, so renaming one makes its prohibited forms invisible;
    # the checker fails CLOSED instead.
    _bucket_mutation "watched symbol renamed out from under the bans" \
        "ALL_BUCKETS = _protocol.ALL_BUCKETS=>ALL_BUCKETS_RENAMED = _protocol.ALL_BUCKETS" \
        "no longer defines"
    # THE TWO EXEMPTIONS MUST NOT BE LOADBEARING HOLES. Both were abusable as
    # first written (re-adversarial round): a QUALIFIED `helper.isinstance(x,
    # Bucket)` satisfied the type-test exemption while its method returned
    # `next(iter(Bucket))`, and the re-export scan exempted EVERY matching
    # top-level assignment rather than one.
    _bucket_mutation "qualified isinstance masquerading as the builtin" \
        "return (None, Bucket.UNSUPPORTED_LANG)=>return (None, _helper.isinstance(None, Bucket))" \
        "bare .Bucket. is used"
    # $'...' so the embedded newline is a REAL newline: inside "..." bash leaves
    # `\n` as a literal backslash-n, which writes an unparseable emitter and
    # makes the fixture pass on rc 3 (infrastructure) instead of the ban it
    # names. Caught by asserting the MESSAGE, not just a non-zero code.
    _bucket_mutation "a second enum re-export binding" \
        $'ALL_BUCKETS = _protocol.ALL_BUCKETS=>ALL_BUCKETS = _protocol.ALL_BUCKETS\nBucket = _alt.Bucket' \
        "re-exported 2 times"
    # A DEAD NESTED DECOY MUST NOT SATISFY THE WATCHED-SYMBOL PROOF. Walking the
    # whole tree counted a binding inside an unused function, so a rename plus a
    # decoy passed while the renamed access evaded the subscript ban.
    _bucket_mutation "dead nested decoy hiding a renamed watched symbol" \
        $'ALL_BUCKETS = _protocol.ALL_BUCKETS=>BUCKETS = _protocol.ALL_BUCKETS\ndef _decoy():\n    ALL_BUCKETS = ()\n    return ALL_BUCKETS' \
        "no longer defines"

    # 22aj: THE CONTRACT MUST HOLD ON THE REAL TREE. A checker that only ever
    # runs against mutated copies proves nothing about what ships.
    python3 "$GATE_REPO/scripts/lint/check_bucket_emission.py" \
        --emitter "$GATE_REPO/scripts/todo-graph/ref_resolution.py" \
        --protocol "$GATE_REPO/scripts/todo-graph/snapshot_protocol.json" \
        >"$TMP_DIR/emit-live.out" 2>"$TMP_DIR/emit-live.err"
    if [ $? -eq 0 ] && grep -q 'contract ok' "$TMP_DIR/emit-live.err"; then
        t_pass "bucket-emission contract: the shipped resolver declares its emitted set"
    else
        t_fail "bucket-emission contract: the shipped resolver violates it (see $TMP_DIR/emit-live.err)"
    fi

    # 22ak: THE RUNTIME HALF. The static bans cannot enumerate every way to
    # compute a name -- an alias, an iteration or a reflective lookup produces a
    # member with no banned shape anywhere in the file (Codex design review,
    # section 20). Once a member leaves the declared set, EVERY route to it must
    # be refused at construction, or the retirement proof is unsound.
    (cd "$GATE_REPO" && python3 - <<'PY2'
import sys
sys.path.insert(0, "scripts/todo-graph")
import ref_resolution as rr

retired = rr.Bucket.PATH_ESCAPE
rr.EMITTED_BUCKETS = frozenset(b for b in rr.EMITTED_BUCKETS if b is not retired)
routes = {
    "alias":      lambda: (lambda B: B.PATH_ESCAPE)(rr.Bucket),
    "iteration":  lambda: [b for b in rr.Bucket if b == "path_escape"][0],
    "reflection": lambda: getattr(rr.Bucket, "PATH_ESCAPE"),
    "by-value":   lambda: rr.Bucket("path_escape"),
    "raw-string": lambda: "path_escape",
}
leaked = []
for label, make in routes.items():
    for ctor in (lambda v: rr.Verdict(None, None, "a", v),
                 lambda v: rr.RefResult(None, "a", v, None, None, None, None)):
        try:
            ctor(make())
            leaked.append(label)
        except rr.BucketContractError:
            pass

# THE NAMEDTUPLE CONSTRUCTION ROUTES. Overriding `__new__` alone left all of
# these open, and a probe drove a retired bucket through every one (Codex
# adversarial, section 20). `_replace` is defined in terms of `_make`, so the
# `_make` override closes both; the module must also bind NO name for an
# unchecked base namedtuple.
ok_v = rr.Verdict(None, None, "a", rr.Bucket.MISSING_FILE)
ok_r = rr.RefResult(None, "a", rr.Bucket.MISSING_FILE, None, None, None, None)
for label, fn in (
        ("Verdict._replace",   lambda: ok_v._replace(bucket=retired)),
        ("Verdict._make",      lambda: rr.Verdict._make((None, None, "a", retired))),
        ("RefResult._replace", lambda: ok_r._replace(bucket=retired)),
        ("RefResult._make",    lambda: rr.RefResult._make(
            (None, "a", retired, None, None, None, None)))):
    try:
        fn()
        leaked.append(label)
    except rr.BucketContractError:
        pass
for base in ("_VerdictBase", "_RefResultBase"):
    if hasattr(rr, base):
        leaked.append("unchecked base %s is importable" % base)

# `tuple.__new__` CANNOT be closed at construction -- nothing in Python can
# take it away -- so the PUBLIC RETURN BOUNDARY is what bounds the observable
# emission set. Assert the boundary, not an impossible constructor guarantee.
smuggled = tuple.__new__(rr.RefResult, (None, "a", retired, None, None, None, None))
# TYPED, because `getattr(result, "bucket", None)` failed OPEN: a missing
# attribute read as the legal RESOLVED value, so a raw tuple carrying an
# undeclared bucket passed the boundary untouched (round 2).
for label, value in (("smuggled RefResult", smuggled),
                     ("raw tuple", (None, "a", retired, None, None, None, None)),
                     ("dict", {"bucket": retired}),
                     ("object without a bucket attribute", object())):
    try:
        rr._bucket_bounded(rr.RefResult)(
            lambda *a, **k: value)({}, rr.EMPTY_SCOPE, None)
        leaked.append("public boundary passed %s" % label)
    except rr.BucketContractError:
        pass
for public in ("classify_ref", "resolve_ref"):
    if not hasattr(getattr(rr, public), "__wrapped__"):
        leaked.append("%s is not bucket-bounded" % public)

# A SUBCLASS CAN CHANGE `bucket` AFTER IT IS VALIDATED. `isinstance` accepted
# one whose property returned a declared member on the boundary's read and a
# RETIRED member on the consumer's next read, so the caller observed a value the
# boundary never approved (post-commit adversarial round). The boundary requires
# the EXACT type.
class _Sneaky(rr.RefResult):
    __slots__ = ()
    _n = [0]

    @property
    def bucket(self):
        self._n[0] += 1
        return rr.Bucket.MISSING_FILE if self._n[0] == 1 else retired

try:
    rr._bucket_bounded(rr.RefResult)(
        lambda *a, **k: _Sneaky(None, "a", rr.Bucket.MISSING_FILE,
                                None, None, None, None))(
        {}, rr.EMPTY_SCOPE, None)
    leaked.append("public boundary passed a bucket-overriding subclass")
except rr.BucketContractError:
    pass
# A still-declared member must keep working, or the check is vacuous.
if rr.Verdict(None, None, "a", rr.Bucket.MISSING_FILE).bucket != "missing_file":
    leaked.append("declared-member-rejected")
if leaked:
    sys.stderr.write("LEAKED: %s\n" % sorted(set(leaked)))
    raise SystemExit(1)
PY2
    ) >"$TMP_DIR/emit-runtime.log" 2>&1
    if [ $? -eq 0 ]; then
        t_pass "bucket-emission contract: a retired member cannot be emitted by any route"
    else
        t_fail "bucket-emission contract: a retired member still reached a verdict (see $TMP_DIR/emit-runtime.log)"
    fi

    # 22al: THE LINT CONSUMER MUST SURVIVE A VOCABULARY MIGRATION. The identity
    # gate could otherwise approve a retirement that leaves lint Check 7
    # unusable: the consumer keys its coverage dict from ALL_BUCKETS but used to
    # retype the six names for its accounting sum, so an emitted ADDITION was
    # counted in one place and not the other (rc 6), and a RETIREMENT raised
    # KeyError on a fixed lookup (Codex design review, section 20). Exercise the
    # consumer itself, not just the gate.
    (cd "$GATE_REPO" && python3 - <<'PY2'
import sys
sys.path.insert(0, "scripts/todo-graph")
sys.path.insert(0, "scripts/lint")
import ref_resolution as rr
import check_stub_behind_stamp as consumer

# ADDITION: a bucket the vocabulary declares and the consumer has never seen.
added = rr.ALL_BUCKETS + ("a_freshly_added_bucket",)
# RETIREMENT: a bucket the consumer used to name directly.
retired = tuple(b for b in rr.ALL_BUCKETS if b != "missing_file")
for label, vocab in (("addition", added), ("retirement", retired)):
    cov = {"occurrences": 0, "resolved": 0}
    cov.update({b: [] for b in vocab})
    cov["occurrences"] = 2
    cov["resolved"] = 1
    cov[vocab[0]].append("x.c:sym")
    accounted = cov["resolved"] + sum(len(cov[b]) for b in vocab)
    if accounted != cov["occurrences"]:
        sys.stderr.write("%s: accounting disagrees (%d vs %d)\n"
                         % (label, accounted, cov["occurrences"]))
        raise SystemExit(1)
    # The reporting path must not name a bucket by hand either.
    labels = {"unresolved_calllike": "calllike-unresolved"}
    line = " ".join(f"{labels.get(b, b)}={len(cov[b])}" for b in vocab)
    if not line:
        sys.stderr.write("%s: empty bucket report\n" % label)
        raise SystemExit(1)
if not hasattr(consumer, "_walk"):
    sys.stderr.write("consumer no longer exposes _walk\n")
    raise SystemExit(1)
PY2
    ) >"$TMP_DIR/consumer-migration.log" 2>&1
    if [ $? -eq 0 ]; then
        t_pass "lint consumer: a bucket addition and a retirement both stay accountable"
    else
        t_fail "lint consumer: a vocabulary migration breaks it (see $TMP_DIR/consumer-migration.log)"
    fi

    # 22z: A DECLARATION ALREADY PRESENT IN BASE pre-authorises the edge it is
    # supposed to declare. Concrete shape (Codex adversarial, round 7): BASE
    # holds bucket `obsolete` AND already declares retired_buckets:
    # ["obsolete"]; HEAD removes the bucket while retaining the declaration.
    # An edge-bound-only rule saw neither a bad declaration nor an undeclared
    # removal and passed.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["post_resolution_buckets"].append("obsolete")
d["retired_buckets"] = ["obsolete"]
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22z-1: base carries the bucket AND its retirement" >/dev/null 2>&1
    )
    G_PREAUTH_BASE="$(cd "$GATE_REPO" && git rev-parse HEAD)"
    (
        cd "$GATE_REPO" || exit 1
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["post_resolution_buckets"].remove("obsolete")   # declaration RETAINED
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22z-2: remove the bucket under the pre-existing declaration" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PREAUTH_BASE" --head HEAD \
        >"$TMP_DIR/gate-22z.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -ne 0 ] && grep -q 'pre-authorises' "$TMP_DIR/gate-22z.log"; then
        t_pass "identity gate: a declaration already present in BASE cannot authorise this edge's removal"
    else
        t_fail "identity gate: pre-existing declaration authorised the removal (rc=$G_RC; see $TMP_DIR/gate-22z.log)"
    fi
    (cd "$GATE_REPO" && git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
        && git commit --quiet --no-verify -am "restore after pre-authorisation fixture" >/dev/null 2>&1)

    # 22ac: THE TRIGGER MUST FIRE ON A CLEAN TREE. GitHub Actions checks out
    # the committed revision with an empty index and worktree, so a trigger
    # reading only `git diff`/`git diff --cached` was ALWAYS false in CI -- the
    # consumer-divergence check never ran in the environment it exists to
    # protect. This extracts lint.sh's own trigger block and runs it against a
    # freshly-initialised clean repo, so it exercises the shipped logic rather
    # than restating it.
    LINT25_TRIG="$TMP_DIR/lint25-trigger.sh"
    LINT25_FAKE="$TMP_DIR/lint25-clean-repo"
    rm -rf "$LINT25_FAKE"; mkdir -p "$LINT25_FAKE"
    # A clean tree WITH THE SUBJECT PRESENT. The trigger is scoped to a tree
    # that actually has the consumer and a todo/ corpus, because lint.sh is
    # also run against scratch repos that have neither -- and Check 25 fails
    # closed, so triggering there turns every such run red. The fixture must
    # model a real checkout, not merely an empty git repo.
    mkdir -p "$LINT25_FAKE/scripts/lint" "$LINT25_FAKE/todo"
    : > "$LINT25_FAKE/scripts/lint/check_stub_behind_stamp.py"
    (cd "$LINT25_FAKE" && git init -q . >/dev/null 2>&1 \
        && git add -A >/dev/null 2>&1 \
        && git -c user.email=t@t.invalid -c user.name=t commit -q -m init >/dev/null 2>&1)
    {
        printf 'REPO_ROOT=%s\n' "$LINT25_FAKE"
        sed -n '/^    LINT25_TOUCHED=0$/,/^    if \[ "\$LINT25_TOUCHED" -eq 1 \]; then$/p' \
            "$REPO_ROOT/scripts/lint.sh" | sed '$d'
        printf 'echo "TOUCHED=$LINT25_TOUCHED"\n'
    } > "$LINT25_TRIG"
    LINT25_TRIG_OUT="$(bash "$LINT25_TRIG" 2>/dev/null | tail -1)"
    if [ "$LINT25_TRIG_OUT" = "TOUCHED=1" ]; then
        t_pass "consumer delegation: the Check 25 trigger fires on a CLEAN tree (i.e. in CI)"
    else
        t_fail "consumer delegation: Check 25 would not run in a clean CI checkout (got '$LINT25_TRIG_OUT')"
    fi

    # 22ab: CHECK 25 MUST FAIL CLOSED. `build/todo-cache.json` is gitignored,
    # so it is absent in a fresh checkout -- which is what CI always is. The
    # first cut routed the resulting rc 3 to a WARNING, so the only gate that
    # can detect the lint consumer diverging from the shared resolution rule
    # passed CI without running, and the identity gate explicitly does not
    # cover the consumer.
    DELEG_NC_RC=0
    STUB_LINT_CACHE="$TMP_DIR/definitely-not-a-cache.json" \
        python3 "$REPO_ROOT/scripts/lint/check_consumer_delegation.py" \
        >"$TMP_DIR/deleg-nocache.log" 2>&1 || DELEG_NC_RC=$?
    if [ "$DELEG_NC_RC" -eq 3 ]; then
        t_pass "consumer delegation: an absent cache is rc 3 (infrastructure), never a pass"
    else
        t_fail "consumer delegation: absent cache did not report infrastructure (rc=$DELEG_NC_RC)"
    fi
    # ...and lint.sh must route that to an ERROR, not a warning, since the
    # branch only runs when a file that can break the invariant changed.
    if sed -n '/^# Check 25: the lint consumer must DELEGATE/,/^# Summary$/p' \
           "$REPO_ROOT/scripts/lint.sh" \
       | grep -q 'Check 25 (consumer-delegation) could not run' \
       && sed -n '/^# Check 25: the lint consumer must DELEGATE/,/^# Summary$/p' \
              "$REPO_ROOT/scripts/lint.sh" \
          | grep -A2 'could not run' | grep -q 'ERRORS=$((ERRORS + 1))'; then
        t_pass "consumer delegation: lint.sh fails CLOSED when the check cannot run"
    else
        t_fail "consumer delegation: lint.sh still warns instead of failing when Check 25 cannot run"
    fi

    # 22aa: THE DEADLINE MUST NOT BE A WALL CLOCK. A wall clock can step
    # BACKWARD, which makes the remaining allowance GROW and hands the later
    # phases more time than the budget permits -- restoring the overrun the
    # single deadline exists to prevent. Not hypothetical on this host: the
    # build-timing test above records three observed backward steps under WSL2
    # on 2026-08-06 after a suspend/resync.
    #
    # THIS IS A STRUCTURAL ASSERTION and says so: it proves the enforcement
    # path is derived from a monotonic source and that no timeout is granted
    # from `date`, NOT that a simulated clock step behaves correctly -- faking
    # the host clock inside the suite is not something a test may do.
    GATE_SRC="$REPO_ROOT/scripts/todo-graph/identity-gate.sh"
    if grep -q 'time.monotonic' "$GATE_SRC" \
       && ! grep -E 'timeout .*--kill-after[^\n]*date \+%s' "$GATE_SRC" >/dev/null \
       && [ "$(grep -c 'remaining()' "$GATE_SRC")" -ge 1 ]; then
        t_pass "identity gate: the phase deadline is derived from a monotonic clock"
    else
        t_fail "identity gate: the phase deadline is not monotonic (a backward wall-clock step would grow the allowance)"
    fi
    if grep -nE 'timeout --kill-after=[0-9]+s "\$BUDGET_SECS"' "$GATE_SRC" >/dev/null; then
        t_fail "identity gate: a phase is still granted the FULL budget instead of the remaining time"
    else
        t_pass "identity gate: every bounded phase is granted only the remaining time"
    fi

    # 22y: a budget of 0 DISABLES timeout(1) outright, so it must be refused
    # before any phase spawns anything -- validating it late left the producer
    # phase, which runs the changed head producer, entirely unbounded.
    # NOTE: the empty string is deliberately NOT in this list. `${VAR:-600}`
    # treats empty as unset, so it takes the documented default -- correct
    # behaviour, not an accepted-bad-value.
    for _bad_budget in 0 abc -1; do
        (cd "$GATE_REPO" && IDENTITY_GATE_BUDGET_SECS="$_bad_budget" \
            bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
            >"$TMP_DIR/gate-22y.log" 2>&1)
        G_RC=$?
        if [ "$G_RC" -eq 2 ] && grep -q 'IDENTITY_GATE_BUDGET_SECS' "$TMP_DIR/gate-22y.log"; then
            t_pass "identity gate: budget '$_bad_budget' is refused as USAGE (2) before any phase runs"
        else
            t_fail "identity gate: budget '$_bad_budget' not refused as usage rc 2 (rc=$G_RC; see $TMP_DIR/gate-22y.log)"
        fi
    done


    # ------------------------------------------------------------------
    # SECTION 50: `UNREADABLE` WAS MANY FACTS WEARING ONE NAME, and the
    # operator was told the wrong one. todo-graph.yml run 32255498885 gated
    # against a base 440 commits back that simply PREDATED the snapshot
    # mechanism, and reported "the snapshot tool is missing or renamed".
    #
    # Each case asserts its own STABLE TOKEN and, where the confusion is the
    # point, asserts the tokens it must NOT be. The cases share one code path,
    # so the controls are what make any of them mean anything: a fix that
    # rescues the ancient base must not drag a mutilated, a half-extracted or
    # a corrupt one along with it.
    # ------------------------------------------------------------------
    # 22ao: A BASE THAT PREDATES THE MECHANISM ENTIRELY. Derived from history
    # rather than hardcoded: take the EARLIEST commit that added any
    # protocol-bearing path and use its parent, which by construction had none.
    # ONE multi-path traversal, not one per path. The clone keeps the full
    # repository history even though its TODO corpus is pruned, so three walks
    # cost three times what the single question needs (Codex perf, section 50
    # post-commit review).
    G_ANCIENT_ADD="$( (cd "$GATE_REPO" && git log --full-history --diff-filter=A \
            --format='%ct %H' HEAD -- \
            scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/ref_resolution.py \
            scripts/todo-graph/corpus_resolution_snapshot.py 2>/dev/null \
        | sort -n | head -1 | awk '{print $2}') )"
    G_ANCIENT=""
    if [ -n "$G_ANCIENT_ADD" ]; then
        G_ANCIENT="$( (cd "$GATE_REPO" && git rev-parse --verify --quiet "${G_ANCIENT_ADD}^" 2>/dev/null) )"
    fi
    if [ -z "$G_ANCIENT" ]; then
        # A SILENT SKIP READS AS A PASS. If the clone cannot supply a
        # pre-protocol commit the case is untested, and that must be visible.
        t_fail "identity gate: could not derive a pre-protocol commit from the fixture clone's history"
    else
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_ANCIENT" --head HEAD \
            >"$TMP_DIR/gate-22ao.log" 2>&1)
        G_RC=$?
        if [ "$G_RC" -eq 3 ] \
           && grep -q 'BASE_PREDATES_PROTOCOL' "$TMP_DIR/gate-22ao.log" \
           && ! grep -qE 'BASE_PROTOCOL_REMOVED|BASE_PROTOCOL_INCOMPLETE' \
                  "$TMP_DIR/gate-22ao.log"; then
            t_pass "identity gate: a base predating the snapshot protocol is named as such, not as a missing tool"
        else
            t_fail "identity gate: pre-protocol base misclassified (rc=$G_RC; see $TMP_DIR/gate-22ao.log)"
        fi
    fi

    # 22an2: THE HALF-EXTRACTED WINDOW IS ITS OWN ANSWER. A COMPLETE protocol
    # is a SET, not a file: this repo's own history added
    # corpus_resolution_snapshot.py at cc05f2e90 and ref_resolution.py fifteen
    # commits later, so every base between them carries one legacy half and no
    # json. An any-path predicate calls that "present but malformed", which is
    # backwards -- nothing is malformed, the extraction was in progress.
    # Reproduced in the clone by leaving exactly one legacy half.
    (
        cd "$GATE_REPO" || exit 1
        git rm -q --ignore-unmatch scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/ref_resolution.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "s50: base carrying one legacy half only" >/dev/null 2>&1
    )
    G_PARTIAL="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/ref_resolution.py >/dev/null 2>&1
        git commit --quiet --no-verify -am "s50: restore the full protocol at head" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PARTIAL" --head HEAD \
        >"$TMP_DIR/gate-22at.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'BASE_PROTOCOL_INCOMPLETE' "$TMP_DIR/gate-22at.log" \
       && ! grep -qE 'BASE_PROTOCOL_UNREADABLE|BASE_PREDATES_PROTOCOL|BASE_PROTOCOL_REMOVED' \
              "$TMP_DIR/gate-22at.log"; then
        t_pass "identity gate: a base carrying only part of the protocol is incomplete, not malformed"
    else
        t_fail "identity gate: partial protocol was forced into a neighbouring verdict (rc=$G_RC; see $TMP_DIR/gate-22at.log)"
    fi

    # 22ap: THE CONTROL THAT MAKES 22ao MEAN SOMETHING. A POST-genesis base
    # that DELETED all three satisfies "none of the paths is present" exactly
    # as an ancient one does, so a filesystem-only predicate would hand it the
    # ancient diagnosis and invite an operator to skip a mutilated range.
    (
        cd "$GATE_REPO" || exit 1
        git rm -q --ignore-unmatch scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/snapshot_protocol.py \
            scripts/todo-graph/ref_resolution.py \
            scripts/todo-graph/corpus_resolution_snapshot.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22ap: base with the protocol removed" >/dev/null 2>&1
    )
    G_REMOVED="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/snapshot_protocol.py \
            scripts/todo-graph/ref_resolution.py \
            scripts/todo-graph/corpus_resolution_snapshot.py >/dev/null 2>&1
        git commit --quiet --no-verify -am "22ap: restore the protocol at head" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_REMOVED" --head HEAD \
        >"$TMP_DIR/gate-22ap.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'BASE_PROTOCOL_REMOVED' "$TMP_DIR/gate-22ap.log" \
       && ! grep -q 'BASE_PREDATES_PROTOCOL' "$TMP_DIR/gate-22ap.log"; then
        t_pass "identity gate: a base that DELETED the protocol is mutilated, never 'ancient'"
    else
        t_fail "identity gate: deleted-protocol base was not distinguished from a pre-protocol one (rc=$G_RC; see $TMP_DIR/gate-22ap.log)"
    fi

    # 22au: A COHERENT RENAME, exercised as an actual rename rather than
    # asserted in a comment. The canonical paths are gone while history still
    # records their addition, which is the same evidence a deletion leaves --
    # and must reach the same verdict, never the ancient one.
    (
        cd "$GATE_REPO" || exit 1
        git mv scripts/todo-graph/snapshot_protocol.json scripts/todo-graph/protocol_data.json >/dev/null 2>&1
        git mv scripts/todo-graph/ref_resolution.py scripts/todo-graph/refres.py >/dev/null 2>&1
        git mv scripts/todo-graph/corpus_resolution_snapshot.py scripts/todo-graph/corpus_snap.py >/dev/null 2>&1
        git mv scripts/todo-graph/snapshot_protocol.py scripts/todo-graph/proto_loader.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22au: base with the protocol renamed away" >/dev/null 2>&1
    )
    G_RENAMED="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        git mv scripts/todo-graph/protocol_data.json scripts/todo-graph/snapshot_protocol.json >/dev/null 2>&1
        git mv scripts/todo-graph/refres.py scripts/todo-graph/ref_resolution.py >/dev/null 2>&1
        git mv scripts/todo-graph/corpus_snap.py scripts/todo-graph/corpus_resolution_snapshot.py >/dev/null 2>&1
        git mv scripts/todo-graph/proto_loader.py scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22au: rename the protocol back at head" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_RENAMED" --head HEAD \
        >"$TMP_DIR/gate-22au.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'BASE_PROTOCOL_REMOVED' "$TMP_DIR/gate-22au.log" \
       && ! grep -q 'BASE_PREDATES_PROTOCOL' "$TMP_DIR/gate-22au.log"; then
        t_pass "identity gate: a RENAMED-away protocol reaches the mutilated verdict, not the ancient one"
    else
        t_fail "identity gate: renamed-away protocol misclassified (rc=$G_RC; see $TMP_DIR/gate-22au.log)"
    fi

    # 22aq: THE CORRUPT-PROTOCOL CONTROL, on the SAME side as 22ao. Present
    # and COMPLETE but unparseable is the original meaning of UNREADABLE and
    # must survive the split intact.
    (
        cd "$GATE_REPO" || exit 1
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["snapshot_schema"] = "not-an-int"
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22aq: base with a malformed protocol" >/dev/null 2>&1
    )
    G_CORRUPT="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json >/dev/null 2>&1
        git commit --quiet --no-verify -am "22aq: restore a well-formed protocol at head" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CORRUPT" --head HEAD \
        >"$TMP_DIR/gate-22aq.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'BASE_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22aq.log" \
       && grep -q 'cannot read the snapshot protocol' "$TMP_DIR/gate-22aq.log" \
       && ! grep -qE 'BASE_PREDATES_PROTOCOL|BASE_PROTOCOL_INCOMPLETE' \
              "$TMP_DIR/gate-22aq.log"; then
        t_pass "identity gate: a base whose protocol is COMPLETE but malformed stays a tree defect"
    else
        t_fail "identity gate: malformed base protocol was reclassified (rc=$G_RC; see $TMP_DIR/gate-22aq.log)"
    fi

    # 22av: WHICH SIDE ANSWERS WHEN BOTH FAIL. The base decides whether the
    # RANGE can be adjudicated at all, so the base token is the documented
    # winner -- and an unpinned ordering is exactly how a head defect starts
    # being reported as a base one.
    (
        cd "$GATE_REPO" || exit 1
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["snapshot_schema"] = "also-not-an-int"
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22av: head unreadable too" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CORRUPT" --head HEAD \
        >"$TMP_DIR/gate-22av.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'BASE_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22av.log" \
       && ! grep -q 'HEAD_PROTOCOL_' "$TMP_DIR/gate-22av.log"; then
        t_pass "identity gate: with BOTH endpoints unreadable the BASE token is the one reported"
    else
        t_fail "identity gate: base-first ordering is not pinned (rc=$G_RC; see $TMP_DIR/gate-22av.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json >/dev/null 2>&1
        git commit --quiet --no-verify -am "22av: restore a well-formed protocol at head" >/dev/null 2>&1
    )

    # 22aw: A FAILED QUESTION IS NOT A NEGATIVE ANSWER. If the history probe
    # cannot run, "never added anywhere in its history" has not been
    # established -- and answering PREDATES from a git failure would let a
    # broken object store manufacture a pristine ancient base. Forced with a
    # PATH shim that fails ONLY the history traversal.
    G_SHIM="$TMP_DIR/gitshim-s50"
    mkdir -p "$G_SHIM"
    G_REAL_GIT="$(command -v git)"
    {
        printf '#!/usr/bin/env bash\n'
        printf 'for a in "$@"; do [ "$a" = "--diff-filter=A" ] && exit 141; done\n'
        printf 'exec %s "$@"\n' "$G_REAL_GIT"
    } > "$G_SHIM/git"
    chmod +x "$G_SHIM/git"
    (cd "$GATE_REPO" && PATH="$G_SHIM:$PATH" bash "$GATE_IN_CLONE" \
        --base "$G_REMOVED" --head HEAD >"$TMP_DIR/gate-22aw.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'BASE_HISTORY_UNREADABLE' "$TMP_DIR/gate-22aw.log" \
       && ! grep -qE 'BASE_PREDATES_PROTOCOL|BASE_PROTOCOL_REMOVED' \
              "$TMP_DIR/gate-22aw.log"; then
        t_pass "identity gate: a git traversal failure refuses instead of becoming classification evidence"
    else
        t_fail "identity gate: failed history probe was read as an answer (rc=$G_RC; see $TMP_DIR/gate-22aw.log)"
    fi

    # 22ax: A TRUNCATED HISTORY CANNOT PROVE A NEGATIVE either. In a shallow
    # clone the ancestry simply stops, so "no add anywhere in its history"
    # describes what this checkout can SEE rather than what happened. The gate
    # must say so rather than infer an ancient base from a horizon.
    #
    # THE BASE MUST ACTUALLY REACH THE BRANCH UNDER TEST. The first cut cloned
    # the fixture repo as it stood and used HEAD~1, whose protocol was intact
    # -- so the gate never entered the classification at all and the case
    # passed while proving nothing. That is the vacuous-pass shape these
    # fixtures exist to refuse, so the removal commit is created FIRST and the
    # depth is chosen to make it the shallow boundary.
    (
        cd "$GATE_REPO" || exit 1
        git rm -q --ignore-unmatch scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/snapshot_protocol.py \
            scripts/todo-graph/ref_resolution.py \
            scripts/todo-graph/corpus_resolution_snapshot.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22ax: the shallow boundary carries no protocol" >/dev/null 2>&1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/snapshot_protocol.py \
            scripts/todo-graph/ref_resolution.py \
            scripts/todo-graph/corpus_resolution_snapshot.py >/dev/null 2>&1
        git commit --quiet --no-verify -am "22ax: restore the protocol at head" >/dev/null 2>&1
    )
    G_SHALLOW="$TMP_DIR/gate-shallow-s50"
    rm -rf "$G_SHALLOW" 2>/dev/null || true
    if git clone --quiet --depth 2 "file://$GATE_REPO" "$G_SHALLOW" >/dev/null 2>&1 \
       && [ -f "$G_SHALLOW/scripts/todo-graph/identity-gate.sh" ]; then
        G_SHALLOW_BASE="$( (cd "$G_SHALLOW" && git rev-parse --verify --quiet HEAD~1 2>/dev/null) )"
        G_IS_SHALLOW="$( (cd "$G_SHALLOW" && git rev-parse --is-shallow-repository 2>/dev/null) )"
        if [ -z "$G_SHALLOW_BASE" ] || [ "$G_IS_SHALLOW" != "true" ]; then
            t_fail "identity gate: the shallow fixture is not actually shallow (base='$G_SHALLOW_BASE' shallow='$G_IS_SHALLOW')"
        else
            (cd "$G_SHALLOW" && bash "$G_SHALLOW/scripts/todo-graph/identity-gate.sh" \
                --base "$G_SHALLOW_BASE" --head HEAD >"$TMP_DIR/gate-22ax.log" 2>&1)
            G_RC=$?
            if [ "$G_RC" -eq 3 ] \
               && grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22ax.log" \
               && ! grep -qE 'BASE_PREDATES_PROTOCOL|BASE_PROTOCOL_REMOVED' \
                      "$TMP_DIR/gate-22ax.log"; then
                t_pass "identity gate: a shallow horizon refuses the pre-protocol inference instead of making it"
            else
                t_fail "identity gate: shallow ancestry was treated as proof of absence (rc=$G_RC; see $TMP_DIR/gate-22ax.log)"
            fi
        fi
    else
        t_fail "identity gate: could not build the shallow fixture clone (a silent skip would read as a pass)"
    fi

    # 22ay: A GENUINE INTRODUCTION WINDOW, taken from real history rather than
    # manufactured by deletion. 22at proves the partial SHAPE is recognised,
    # but its missing paths were added in visible history, so it exercises only
    # the "mutilated" half of that arm. The commit that FIRST added any
    # protocol path carries exactly one of them and its ancestry never added
    # the others -- the half-extracted case the live incident actually spans.
    if [ -z "$G_ANCIENT_ADD" ]; then
        t_fail "identity gate: no first-protocol-add commit to test the introduction window with"
    else
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_ANCIENT_ADD" --head HEAD \
            >"$TMP_DIR/gate-22ay.log" 2>&1)
        G_RC=$?
        if [ "$G_RC" -eq 3 ] \
           && grep -q 'BASE_PROTOCOL_INCOMPLETE' "$TMP_DIR/gate-22ay.log" \
           && grep -q 'never added anywhere in its history' "$TMP_DIR/gate-22ay.log" \
           && ! grep -q 'mutilated' "$TMP_DIR/gate-22ay.log"; then
            t_pass "identity gate: a half-extracted base is named as early, not as mutilated"
        else
            t_fail "identity gate: the introduction window took the mutilated branch (rc=$G_RC; see $TMP_DIR/gate-22ay.log)"
        fi
    fi

    # 22az: THE PARTIAL ARM MUST CLEAR THE SAME HISTORY BAR AS THE NONE ARM.
    # It did not: a shallow boundary whose base happened to retain one protocol
    # path was told "the extraction was still in progress. Nothing is wrong
    # with the tree under test" -- an inference drawn from a horizon.
    (
        cd "$GATE_REPO" || exit 1
        git rm -q --ignore-unmatch scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/ref_resolution.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22az: shallow boundary keeps one legacy half" >/dev/null 2>&1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/ref_resolution.py >/dev/null 2>&1
        git commit --quiet --no-verify -am "22az: restore the protocol at head" >/dev/null 2>&1
    )
    G_SHALLOW2="$TMP_DIR/gate-shallow2-s50"
    rm -rf "$G_SHALLOW2" 2>/dev/null || true
    if git clone --quiet --depth 2 "file://$GATE_REPO" "$G_SHALLOW2" >/dev/null 2>&1 \
       && [ -f "$G_SHALLOW2/scripts/todo-graph/identity-gate.sh" ]; then
        G_SB2="$( (cd "$G_SHALLOW2" && git rev-parse --verify --quiet HEAD~1 2>/dev/null) )"
        if [ -z "$G_SB2" ]; then
            t_fail "identity gate: the shallow partial fixture has no boundary commit"
        else
            (cd "$G_SHALLOW2" && bash "$G_SHALLOW2/scripts/todo-graph/identity-gate.sh" \
                --base "$G_SB2" --head HEAD >"$TMP_DIR/gate-22az.log" 2>&1)
            G_RC=$?
            if [ "$G_RC" -eq 3 ] \
               && grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22az.log" \
               && ! grep -q 'Nothing is wrong with the tree under test' "$TMP_DIR/gate-22az.log"; then
                t_pass "identity gate: a PARTIAL base over a shallow ancestry refuses instead of inferring"
            else
                t_fail "identity gate: the partial arm drew a never-added inference from a horizon (rc=$G_RC; see $TMP_DIR/gate-22az.log)"
            fi
        fi
    else
        t_fail "identity gate: could not build the shallow partial fixture clone"
    fi

    # 22ba: A FAILED HEAD TREE PROBE IS A REPOSITORY FAILURE, NOT A TREE
    # DEFECT. Folded into an `&&`, a git failure merely made the condition
    # false and fell through to a HEAD_PROTOCOL_* token, blaming the tree under
    # test for a question the repository could not answer. Forced with a shim
    # that fails ls-tree ONLY for the head commit.
    # THE REMOVAL IS COMMITTED, not left in the worktree (section 54). This
    # case needs `proto_of` to FAIL so the classification branch is entered at
    # all; since the head side reads the commit, only a committed removal gets
    # it there. Removing from the worktree alone now changes nothing, which is
    # the property 22as asserts.
    (
        cd "$GATE_REPO" || exit 1
        git rm --quiet -f scripts/todo-graph/snapshot_protocol.json \
              scripts/todo-graph/snapshot_protocol.py \
              scripts/todo-graph/ref_resolution.py \
              scripts/todo-graph/corpus_resolution_snapshot.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22ba: the protocol is gone from the commit" >/dev/null 2>&1
    )
    G_HEAD_SHA="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    G_SHIM2="$TMP_DIR/gitshim2-s50"
    mkdir -p "$G_SHIM2"
    {
        printf '#!/usr/bin/env bash\n'
        printf 'if [ "$1" = "ls-tree" ]; then\n'
        printf '  for a in "$@"; do [ "$a" = "%s" ] && exit 128; done\n' "$G_HEAD_SHA"
        printf 'fi\n'
        printf 'exec %s "$@"\n' "$G_REAL_GIT"
    } > "$G_SHIM2/git"
    chmod +x "$G_SHIM2/git"
    (cd "$GATE_REPO" && PATH="$G_SHIM2:$PATH" bash "$GATE_IN_CLONE" \
        --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22ba.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'HEAD_TREE_UNREADABLE' "$TMP_DIR/gate-22ba.log" \
       && ! grep -qE 'HEAD_PROTOCOL_ABSENT|HEAD_PROTOCOL_INCOMPLETE' \
              "$TMP_DIR/gate-22ba.log"; then
        t_pass "identity gate: an unreadable HEAD tree is a repository failure, not a tree defect"
    else
        t_fail "identity gate: failed head tree probe was blamed on the tree (rc=$G_RC; see $TMP_DIR/gate-22ba.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/snapshot_protocol.py \
            scripts/todo-graph/ref_resolution.py \
            scripts/todo-graph/corpus_resolution_snapshot.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22ba: restore the protocol in the commit" >/dev/null 2>&1
    )

    # 22bb: THE COMPLETENESS PROBE ITSELF MUST FAIL CLOSED. `history_is_complete`
    # ignored the exit status of its own git calls, so a FAILED probe read as
    # "not shallow, no replacements" and cleared the way for the ancient-base
    # inference -- the failed-question-as-negative-answer fault one level down.
    G_SHIM3="$TMP_DIR/gitshim3-s50"
    mkdir -p "$G_SHIM3"
    {
        printf '#!/usr/bin/env bash\n'
        printf '[ "$1" = "replace" ] && exit 129\n'
        printf 'exec %s "$@"\n' "$G_REAL_GIT"
    } > "$G_SHIM3/git"
    chmod +x "$G_SHIM3/git"
    (cd "$GATE_REPO" && PATH="$G_SHIM3:$PATH" bash "$GATE_IN_CLONE" \
        --base "$G_REMOVED" --head HEAD >"$TMP_DIR/gate-22bb.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22bb.log" \
       && ! grep -qE 'BASE_PREDATES_PROTOCOL|BASE_PROTOCOL_REMOVED' \
              "$TMP_DIR/gate-22bb.log"; then
        t_pass "identity gate: a failed history-completeness probe fails closed"
    else
        t_fail "identity gate: completeness probe failure was read as 'history is fine' (rc=$G_RC; see $TMP_DIR/gate-22bb.log)"
    fi

    # 22bc: A DIRECTORY IS NOT A PROTOCOL FILE. `git ls-tree -r` expands a
    # DIRECTORY pathspec to its descendants, so a directory named
    # snapshot_protocol.json yields `.../snapshot_protocol.json/README` -- and
    # a substring test over the joined result read that as the FILE being
    # present, making a mutilated tree look `complete` and earning it the
    # unreadable token instead of the removed one. Membership is now decided
    # from each path's own entry, and the entry must be a blob.
    (
        cd "$GATE_REPO" || exit 1
        git rm -q --ignore-unmatch scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/snapshot_protocol.py \
            scripts/todo-graph/ref_resolution.py \
            scripts/todo-graph/corpus_resolution_snapshot.py >/dev/null 2>&1
        mkdir -p scripts/todo-graph/snapshot_protocol.json
        printf 'not a protocol\n' > scripts/todo-graph/snapshot_protocol.json/README
        git add scripts/todo-graph/snapshot_protocol.json/README >/dev/null 2>&1
        git commit --quiet --no-verify -m "22bc: a directory wearing the protocol file's name" >/dev/null 2>&1
    )
    G_DIRBASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        git rm -q -r --ignore-unmatch scripts/todo-graph/snapshot_protocol.json >/dev/null 2>&1
        rm -rf scripts/todo-graph/snapshot_protocol.json
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/snapshot_protocol.py \
            scripts/todo-graph/ref_resolution.py \
            scripts/todo-graph/corpus_resolution_snapshot.py >/dev/null 2>&1
        git commit --quiet --no-verify -am "22bc: restore the real protocol at head" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_DIRBASE" --head HEAD \
        >"$TMP_DIR/gate-22bc.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'BASE_PROTOCOL_REMOVED' "$TMP_DIR/gate-22bc.log" \
       && ! grep -qE 'BASE_PROTOCOL_UNREADABLE|BASE_PROTOCOL_INCOMPLETE' \
              "$TMP_DIR/gate-22bc.log"; then
        t_pass "identity gate: a directory wearing a protocol file's name is not the protocol"
    else
        t_fail "identity gate: a directory was counted as the protocol file (rc=$G_RC; see $TMP_DIR/gate-22bc.log)"
    fi

    # 22bd: A CUSTOM GRAFT FILE IS STILL A TRUNCATED HISTORY. `GIT_GRAFT_FILE`
    # overrides `info/grafts` and is inherited by the traversal, so checking
    # only git's default file left a non-default graft free to hide the commit
    # that introduced a protocol path while the default sat empty -- and the
    # negative traversal that followed then read as proof.
    G_GRAFT="$TMP_DIR/s50-grafts"
    printf '%s\n' "$G_REMOVED" > "$G_GRAFT"
    (cd "$GATE_REPO" && GIT_GRAFT_FILE="$G_GRAFT" bash "$GATE_IN_CLONE" \
        --base "$G_REMOVED" --head HEAD >"$TMP_DIR/gate-22bd.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22bd.log" \
       && ! grep -qE 'BASE_PREDATES_PROTOCOL|BASE_PROTOCOL_REMOVED' \
              "$TMP_DIR/gate-22bd.log"; then
        t_pass "identity gate: a custom GIT_GRAFT_FILE is refused, not walked around"
    else
        t_fail "identity gate: a non-default graft hid the protocol introduction (rc=$G_RC; see $TMP_DIR/gate-22bd.log)"
    fi
    # MUTATION CONTROL: the very same base and head WITHOUT the graft must
    # reach a real verdict, or 22bd would pass for the wrong reason.
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_REMOVED" --head HEAD \
        >"$TMP_DIR/gate-22bd-control.log" 2>&1)
    if grep -q 'BASE_PROTOCOL_REMOVED' "$TMP_DIR/gate-22bd-control.log"; then
        t_pass "identity gate: CONTROL -- without the graft the same range classifies normally"
    else
        t_fail "identity gate: the 22bd control did not reach its normal verdict (see $TMP_DIR/gate-22bd-control.log)"
    fi

    # 22be: A FOURTH PROTOCOL PATH MUST NOT BE SILENTLY IGNORED. The presence
    # flags were a fixed three-element array indexed by literals beside a
    # SEPARATE path array, so appending a path would have run cleanly while
    # that path was invisible to presence, form and missing-path. Mutation
    # applied to the CLONE's own copy of the gate, never the live one.
    G_MUT="$GATE_REPO/scripts/todo-graph/identity-gate-mut.sh"
    sed 's#^    "\$PROTOCOL_LEGACY_SNAPSHOT"$#    "$PROTOCOL_LEGACY_SNAPSHOT"\n    "scripts/todo-graph/protocol_addendum.py"#' \
        "$GATE_IN_CLONE" > "$G_MUT"
    if ! grep -q 'protocol_addendum.py' "$G_MUT"; then
        t_fail "identity gate: could not build the fourth-path mutation (the PROTOCOL_PATHS anchor moved)"
    else
        (cd "$GATE_REPO" && bash "$G_MUT" --base "$G_REMOVED" --head HEAD \
            >"$TMP_DIR/gate-22be.log" 2>&1)
        G_RC=$?
        # THE EXACT SIDE, not the shared suffix: this case is constructed so the
        # BASE answers first, and greping `FORM_UNMAPPED` would stay green if an
        # ordering regression made the HEAD answer instead.
        if [ "$G_RC" -eq 3 ] \
           && grep -q 'BASE_FORM_UNMAPPED' "$TMP_DIR/gate-22be.log" \
           && ! grep -q 'HEAD_FORM_UNMAPPED' "$TMP_DIR/gate-22be.log"; then
            t_pass "identity gate: a protocol path the form mapping does not cover is refused, not ignored"
        else
            t_fail "identity gate: a fourth protocol path was silently ignored (rc=$G_RC; see $TMP_DIR/gate-22be.log)"
        fi
    fi
    rm -f "$G_MUT"

    # 22bf: A SYMLINK IS NOT A PROTOCOL FILE. Git stores one as mode 120000,
    # type blob, so accepting every blob made a committed DANGLING symlink look
    # present on the commit side while the worktree probe's `-f` rejected it.
    # The two sides disagreed and the gate blamed the CHECKOUT for a defect in
    # the committed tree -- the right refusal class, the wrong remediation.
    (
        cd "$GATE_REPO" || exit 1
        git rm -q --ignore-unmatch scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/snapshot_protocol.py \
            scripts/todo-graph/ref_resolution.py \
            scripts/todo-graph/corpus_resolution_snapshot.py >/dev/null 2>&1
        ln -s nowhere-at-all scripts/todo-graph/snapshot_protocol.json
        git add scripts/todo-graph/snapshot_protocol.json >/dev/null 2>&1
        git commit --quiet --no-verify -m "22bf: the protocol path is a dangling symlink" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22bf.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'HEAD_PROTOCOL_ABSENT' "$TMP_DIR/gate-22bf.log" \
       && ! grep -q 'HEAD_PROTOCOL_UNMATERIALIZED' "$TMP_DIR/gate-22bf.log"; then
        t_pass "identity gate: a committed dangling symlink is a tree defect, not an incomplete checkout"
    else
        t_fail "identity gate: a symlink blob was counted as a protocol file (rc=$G_RC; see $TMP_DIR/gate-22bf.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        git rm -q --ignore-unmatch scripts/todo-graph/snapshot_protocol.json >/dev/null 2>&1
        rm -f scripts/todo-graph/snapshot_protocol.json
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/snapshot_protocol.py \
            scripts/todo-graph/ref_resolution.py \
            scripts/todo-graph/corpus_resolution_snapshot.py >/dev/null 2>&1
        git commit --quiet --no-verify -am "22bf: restore the real protocol at head" >/dev/null 2>&1
    )

    # 22bg: AN UNRELATED REPLACEMENT REF MUST NOT CONDEMN THE RANGE. Any
    # `git replace` entry used to mark the whole repository incomplete, so a
    # replacement for a commit the base's own traversal can neither reach nor
    # consult produced BASE_HISTORY_INCOMPLETE for a base whose history was
    # entirely intact.
    #
    # BUILT WITH PLUMBING, NOT AN ORPHAN BRANCH. The first cut used
    # `git checkout --orphan` plus `git rm -rf .`, which emptied the shared
    # fixture worktree; the return checkout did not restore it and the next
    # FOUR cases died at rc 127 with the gate script simply gone. `commit-tree`
    # writes unreachable commits without touching the worktree at all -- and
    # the replacement ref that follows is what keeps them alive.
    G_EMPTY_TREE="$( (cd "$GATE_REPO" && git hash-object -t tree /dev/null 2>/dev/null) )"
    G_ORPHAN_A=""
    G_ORPHAN_B=""
    if [ -n "$G_EMPTY_TREE" ]; then
        G_ORPHAN_A="$( (cd "$GATE_REPO" && git commit-tree "$G_EMPTY_TREE" -m "22bg: unreachable A" </dev/null 2>/dev/null) )"
        [ -n "$G_ORPHAN_A" ] && G_ORPHAN_B="$( (cd "$GATE_REPO" && git commit-tree "$G_EMPTY_TREE" -p "$G_ORPHAN_A" -m "22bg: unreachable B" </dev/null 2>/dev/null) )"
    fi
    if [ -z "$G_ORPHAN_A" ] || [ -z "$G_ORPHAN_B" ]; then
        t_fail "identity gate: could not build the unrelated-replacement fixture (no unreachable commits)"
    else
        (cd "$GATE_REPO" && git replace -f "$G_ORPHAN_A" "$G_ORPHAN_B" >/dev/null 2>&1)
        if [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
            t_fail "identity gate: the unrelated replacement ref was not created (the case would pass vacuously)"
        else
            (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_REMOVED" --head HEAD \
                >"$TMP_DIR/gate-22bg.log" 2>&1)
            G_RC=$?
            if [ "$G_RC" -eq 3 ] \
               && grep -q 'BASE_PROTOCOL_REMOVED' "$TMP_DIR/gate-22bg.log" \
               && ! grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22bg.log"; then
                t_pass "identity gate: a replacement ref outside the base ancestry leaves the verdict alone"
            else
                t_fail "identity gate: unrelated repository metadata condemned the range (rc=$G_RC; see $TMP_DIR/gate-22bg.log)"
            fi
            # AND THE POSITIVE HALF: a replacement INSIDE the base ancestry
            # must still refuse, or the scoping would have removed the guard
            # rather than narrowed it.
            (cd "$GATE_REPO" && git replace -f "$G_REMOVED" "$G_ORPHAN_B" >/dev/null 2>&1)
            (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_REMOVED" --head HEAD \
                >"$TMP_DIR/gate-22bg2.log" 2>&1)
            G_RC=$?
            if [ "$G_RC" -eq 3 ] && grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22bg2.log"; then
                t_pass "identity gate: a replacement ref INSIDE the base ancestry still refuses"
            else
                t_fail "identity gate: scoping removed the replacement guard entirely (rc=$G_RC; see $TMP_DIR/gate-22bg2.log)"
            fi
            (cd "$GATE_REPO" && git replace -d "$G_ORPHAN_A" >/dev/null 2>&1
             cd "$GATE_REPO" && git replace -d "$G_REMOVED" >/dev/null 2>&1) >/dev/null 2>&1
        fi
    fi
    # 22bh: A REPLACEMENT OF THE BASE OBJECT ITSELF. Distinct from 22bg, which
    # is about a replacement elsewhere in the ancestry: replacing the BASE
    # substitutes the very tree every presence probe reads. A synthetic tree
    # carrying a COMPLETE but malformed protocol therefore reached the
    # `complete` arm -- which never consults the completeness guard -- and
    # earned BASE_PROTOCOL_UNREADABLE, a statement about a tree the repository
    # manufactured rather than one stored at that commit.
    if [ -z "$G_CORRUPT" ]; then
        t_fail "identity gate: no malformed-protocol commit available to replace the base with"
    else
        (cd "$GATE_REPO" && git replace -f "$G_PROTO_BASE" "$G_CORRUPT" >/dev/null 2>&1)
        if [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
            t_fail "identity gate: the base replacement was not created (the case would pass vacuously)"
        else
            (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
                >"$TMP_DIR/gate-22bh.log" 2>&1)
            G_RC=$?
            if [ "$G_RC" -eq 3 ] \
               && grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22bh.log" \
               && ! grep -q 'BASE_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22bh.log"; then
                t_pass "identity gate: a REPLACED base object is repository metadata, not a malformed tree"
            else
                t_fail "identity gate: a manufactured base tree was classified as the stored one (rc=$G_RC; see $TMP_DIR/gate-22bh.log)"
            fi
            (cd "$GATE_REPO" && git replace -d "$G_PROTO_BASE" >/dev/null 2>&1)
        fi
    fi
    # MUTATION CONTROL: with the replacement removed, the SAME invocation must
    # reach a real verdict again -- otherwise 22bh proves only that something
    # broke.
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CORRUPT" --head HEAD \
        >"$TMP_DIR/gate-22bh-control.log" 2>&1)
    if grep -q 'BASE_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22bh-control.log"; then
        t_pass "identity gate: CONTROL -- an unreplaced malformed base still reads as malformed"
    else
        t_fail "identity gate: the 22bh control lost its normal verdict (see $TMP_DIR/gate-22bh-control.log)"
    fi

    # 22bi: THE REPLACEMENT NAMESPACE IS CONFIGURABLE. `GIT_REPLACE_REF_BASE`
    # relocates where git looks for replacements, so the base-replacement guard
    # -- which looked under a hardcoded `refs/replace/` -- missed a replacement
    # git was actively honouring, and the manufactured tree was classified as
    # the stored one all over again.
    if [ -z "$G_CORRUPT" ]; then
        t_fail "identity gate: no malformed-protocol commit available for the namespace case"
    else
        (cd "$GATE_REPO" && git update-ref "refs/replacements/$G_PROTO_BASE" "$G_CORRUPT" >/dev/null 2>&1)
        if [ -z "$( (cd "$GATE_REPO" && git rev-parse --verify --quiet "refs/replacements/$G_PROTO_BASE" 2>/dev/null) )" ]; then
            t_fail "identity gate: the relocated replacement ref was not created (the case would pass vacuously)"
        else
            (cd "$GATE_REPO" && GIT_REPLACE_REF_BASE=refs/replacements/ \
                bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
                >"$TMP_DIR/gate-22bi.log" 2>&1)
            G_RC=$?
            if [ "$G_RC" -eq 3 ] \
               && grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22bi.log" \
               && ! grep -q 'BASE_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22bi.log"; then
                t_pass "identity gate: a relocated replacement namespace is honoured, not missed"
            else
                t_fail "identity gate: GIT_REPLACE_REF_BASE bypassed the base-replacement guard (rc=$G_RC; see $TMP_DIR/gate-22bi.log)"
            fi
            (cd "$GATE_REPO" && git update-ref -d "refs/replacements/$G_PROTO_BASE" >/dev/null 2>&1)
        fi
    fi

    # 22bj: A DANGLING-BLOB REPLACEMENT REACHES NOTHING. `merge-base
    # --is-ancestor` accepts only commits and exits 128 for a blob, which the
    # fail-closed arm read as "could not decide" -- so one `git replace` of two
    # unreachable blobs condemned every range in the repository. Scoping is now
    # object-type aware; a provably unreachable non-commit original is ignored.
    G_BLOB_A="$( (cd "$GATE_REPO" && printf 'dangling one\n' | git hash-object -w --stdin 2>/dev/null) )"
    G_BLOB_B="$( (cd "$GATE_REPO" && printf 'dangling two\n' | git hash-object -w --stdin 2>/dev/null) )"
    if [ -z "$G_BLOB_A" ] || [ -z "$G_BLOB_B" ]; then
        t_fail "identity gate: could not write the dangling blobs for the non-commit replacement case"
    else
        (cd "$GATE_REPO" && git replace -f "$G_BLOB_A" "$G_BLOB_B" >/dev/null 2>&1)
        if [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
            t_fail "identity gate: the dangling-blob replacement was not created (the case would pass vacuously)"
        else
            (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_REMOVED" --head HEAD \
                >"$TMP_DIR/gate-22bj.log" 2>&1)
            G_RC=$?
            if [ "$G_RC" -eq 3 ] \
               && grep -q 'BASE_PROTOCOL_REMOVED' "$TMP_DIR/gate-22bj.log" \
               && ! grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22bj.log"; then
                t_pass "identity gate: an unreachable blob replacement does not condemn the range"
            else
                t_fail "identity gate: a dangling-blob replacement blocked an intact history (rc=$G_RC; see $TMP_DIR/gate-22bj.log)"
            fi
            # THE POSITIVE HALF: a REACHABLE blob replacement must still
            # refuse, or the type-awareness would have deleted the guard for
            # every non-commit rather than narrowing it.
            G_LIVE_BLOB="$( (cd "$GATE_REPO" && git rev-parse "$G_REMOVED:scripts/todo-graph/build.py" 2>/dev/null) )"
            if [ -z "$G_LIVE_BLOB" ]; then
                t_fail "identity gate: could not find a reachable blob for the positive half"
            else
                (cd "$GATE_REPO" && git replace -f "$G_LIVE_BLOB" "$G_BLOB_B" >/dev/null 2>&1)
                (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_REMOVED" --head HEAD \
                    >"$TMP_DIR/gate-22bj2.log" 2>&1)
                G_RC=$?
                G_BJ2_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bj2.log" | head -1)"
                if [ "$G_RC" -eq 3 ] && grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22bj2.log"; then
                    t_pass "identity gate: a REACHABLE blob replacement still refuses"
                else
                    t_fail "identity gate: type-awareness removed the non-commit guard entirely (rc=$G_RC; got [$G_BJ2_TOKEN]; see $TMP_DIR/gate-22bj2.log)"
                fi
                (cd "$GATE_REPO" && git replace -d "$G_LIVE_BLOB" >/dev/null 2>&1)
            fi
            (cd "$GATE_REPO" && git replace -d "$G_BLOB_A" >/dev/null 2>&1)
        fi
    fi

    # 22bk: A SUBSTITUTED PROTOCOL BLOB UNDER A VALID BASE. The replacement
    # question used to live inside the completeness helper, which only the arms
    # making a never-added inference ever called -- so the `complete` arm,
    # which dies immediately, never asked it. Replacing a VALID base's own
    # snapshot_protocol.json blob with a malformed one of the same type made
    # `proto_of` fail while `ls-tree` still reported a perfectly good regular
    # blob, and a stored base that is entirely valid was called malformed.
    # A BENIGN CLOSURE CHANGE FIRST. Without one the closure is byte-identical
    # base-vs-head and the gate exits 0 before the protocol is ever read, so
    # the case would report a pass having tested nothing. (That early exit is
    # section 51's subject; here it is only in the way.)
    (
        cd "$GATE_REPO" || exit 1
        printf '\n# 22bk: benign closure change, no behaviour\n' >> scripts/todo-graph/build.py
        git commit --quiet --no-verify -am "22bk: benign closure change" >/dev/null 2>&1
    )
    G_BAD_JSON="$( (cd "$GATE_REPO" && printf 'not json at all\n' | git hash-object -w --stdin 2>/dev/null) )"
    G_GOOD_JSON="$( (cd "$GATE_REPO" && git rev-parse "$G_PROTO_BASE:scripts/todo-graph/snapshot_protocol.json" 2>/dev/null) )"
    if [ -z "$G_BAD_JSON" ] || [ -z "$G_GOOD_JSON" ]; then
        t_fail "identity gate: could not build the substituted-protocol-blob fixture"
    else
        (cd "$GATE_REPO" && git replace -f "$G_GOOD_JSON" "$G_BAD_JSON" >/dev/null 2>&1)
        if [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
            t_fail "identity gate: the protocol-blob replacement was not created (the case would pass vacuously)"
        else
            (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
                >"$TMP_DIR/gate-22bk.log" 2>&1)
            G_RC=$?
            G_BK_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bk.log" | head -1)"
            if [ "$G_RC" -eq 3 ] \
               && grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22bk.log" \
               && ! grep -q 'BASE_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22bk.log"; then
                t_pass "identity gate: a substituted protocol BLOB is repository metadata, not a malformed base"
            else
                t_fail "identity gate: a valid base was called malformed because of a blob replacement (rc=$G_RC; got [$G_BK_TOKEN]; see $TMP_DIR/gate-22bk.log)"
            fi
            (cd "$GATE_REPO" && git replace -d "$G_GOOD_JSON" >/dev/null 2>&1)
        fi
    fi
    # RESTORE build.py. Leaving it changed relative to G_PROTO_BASE made every
    # LATER protocol-only case enter the producer differential -- four producer
    # builds over two corpora -- to assert something about record transport
    # (Codex perf, section 50 post-commit review). Cases that need the closure
    # to differ still add their own benign change.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/build.py >/dev/null 2>&1
        git commit --quiet --no-verify -am "22bk: restore build.py" >/dev/null 2>&1
    )

    # 22bl: A LISTED REPLACEMENT IS NOT NECESSARILY AN ACTIVE ONE. Git stops
    # reading replacements under `GIT_NO_REPLACE_OBJECTS` or
    # `core.useReplaceRefs=false`, so every read resolves stored objects while
    # `git replace -l` still enumerates the refs -- and a base that was never
    # substituted was refused as manufactured. Both knobs are exercised,
    # because they are separate mechanisms and only one of them is an
    # environment variable.
    G_LIVE_BLOB2="$( (cd "$GATE_REPO" && git rev-parse "$G_REMOVED:scripts/todo-graph/build.py" 2>/dev/null) )"
    G_SPARE_BLOB="$( (cd "$GATE_REPO" && printf 'spare\n' | git hash-object -w --stdin 2>/dev/null) )"
    if [ -z "$G_LIVE_BLOB2" ] || [ -z "$G_SPARE_BLOB" ]; then
        t_fail "identity gate: could not build the disabled-replacement fixture"
    else
        (cd "$GATE_REPO" && git replace -f "$G_LIVE_BLOB2" "$G_SPARE_BLOB" >/dev/null 2>&1)
        if [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
            t_fail "identity gate: the replacement for the disabled case was not created"
        else
            # CONTROL FIRST: with replacements ENABLED this same state must
            # refuse, or the two assertions below would prove nothing.
            (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_REMOVED" --head HEAD \
                >"$TMP_DIR/gate-22bl-control.log" 2>&1)
            if grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22bl-control.log"; then
                t_pass "identity gate: CONTROL -- with replacements enabled the same state refuses"
            else
                t_fail "identity gate: the 22bl control did not refuse (see $TMP_DIR/gate-22bl-control.log)"
            fi
            (cd "$GATE_REPO" && GIT_NO_REPLACE_OBJECTS=1 bash "$GATE_IN_CLONE" \
                --base "$G_REMOVED" --head HEAD >"$TMP_DIR/gate-22bl.log" 2>&1)
            G_RC=$?
            G_BL_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bl.log" | head -1)"
            if [ "$G_RC" -eq 3 ] \
               && grep -q 'BASE_PROTOCOL_REMOVED' "$TMP_DIR/gate-22bl.log" \
               && ! grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22bl.log"; then
                t_pass "identity gate: a replacement git is not reading does not condemn the range"
            else
                t_fail "identity gate: a disabled replacement was treated as active (rc=$G_RC; got [$G_BL_TOKEN]; see $TMP_DIR/gate-22bl.log)"
            fi
            # PRESENCE, NOT TRUTHINESS: git disables replacements for ANY set
            # value, so `=0` must reach the same verdict as `=1`. Recognising
            # only truthy words refused ranges git was not substituting.
            (cd "$GATE_REPO" && GIT_NO_REPLACE_OBJECTS=0 bash "$GATE_IN_CLONE" \
                --base "$G_REMOVED" --head HEAD >"$TMP_DIR/gate-22bl0.log" 2>&1)
            G_RC=$?
            G_BL0_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bl0.log" | head -1)"
            if [ "$G_RC" -eq 3 ] \
               && grep -q 'BASE_PROTOCOL_REMOVED' "$TMP_DIR/gate-22bl0.log" \
               && ! grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22bl0.log"; then
                t_pass "identity gate: GIT_NO_REPLACE_OBJECTS=0 disables replacements exactly as =1 does"
            else
                t_fail "identity gate: a falsey-but-SET disable value was read as ambiguous (rc=$G_RC; got [$G_BL0_TOKEN]; see $TMP_DIR/gate-22bl0.log)"
            fi
            (cd "$GATE_REPO" && git -c core.useReplaceRefs=false config core.useReplaceRefs false >/dev/null 2>&1)
            (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_REMOVED" --head HEAD \
                >"$TMP_DIR/gate-22bl2.log" 2>&1)
            G_RC=$?
            G_BL2_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bl2.log" | head -1)"
            if [ "$G_RC" -eq 3 ] \
               && grep -q 'BASE_PROTOCOL_REMOVED' "$TMP_DIR/gate-22bl2.log" \
               && ! grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22bl2.log"; then
                t_pass "identity gate: core.useReplaceRefs=false is honoured the same as the env knob"
            else
                t_fail "identity gate: the config knob was ignored (rc=$G_RC; got [$G_BL2_TOKEN]; see $TMP_DIR/gate-22bl2.log)"
            fi
            (cd "$GATE_REPO" && git config --unset core.useReplaceRefs >/dev/null 2>&1
             cd "$GATE_REPO" && git replace -d "$G_LIVE_BLOB2" >/dev/null 2>&1) >/dev/null 2>&1
        fi
    fi

    # 22bm: A RESOLVABLE SYMLINK PARSES, AND THAT IS THE POINT. 22bf covers a
    # DANGLING symlink, whose failed parse enters the unreadable branch where
    # the file-type checks live -- so it cannot see this. `proto_of` reads with
    # `pathlib.is_file()` and `read_text()`, both of which FOLLOW symlinks, so
    # a protocol path symlinked to valid JSON parses cleanly, skips those
    # branches entirely, and the gate adjudicates a range through bytes the
    # commit need not even contain.
    (
        cd "$GATE_REPO" || exit 1
        cp scripts/todo-graph/snapshot_protocol.json scripts/todo-graph/protocol_real.json
        rm -f scripts/todo-graph/snapshot_protocol.json
        ln -s protocol_real.json scripts/todo-graph/snapshot_protocol.json
        git add -A scripts/todo-graph >/dev/null 2>&1
        printf '\n# 22bm: benign closure change, no behaviour\n' >> scripts/todo-graph/build.py
        git add scripts/todo-graph/build.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22bm: the protocol path is a resolvable symlink" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22bm.log" 2>&1)
    G_RC=$?
    G_BM_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bm.log" | head -1)"
    if [ "$G_RC" -eq 3 ] && grep -q 'HEAD_PROTOCOL_NOT_A_FILE' "$TMP_DIR/gate-22bm.log"; then
        t_pass "identity gate: a protocol that PARSES through a symlink is still refused"
    else
        t_fail "identity gate: a resolvable symlink reached a verdict (rc=$G_RC; got [$G_BM_TOKEN]; see $TMP_DIR/gate-22bm.log)"
    fi
    # THE BASE SIDE OF THE SAME CONTRACT, which is checked in the COMMIT rather
    # than the working tree and so needs its own case.
    G_SYMBASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        rm -f scripts/todo-graph/snapshot_protocol.json
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json >/dev/null 2>&1
        git rm -q --ignore-unmatch scripts/todo-graph/protocol_real.json >/dev/null 2>&1
        git add -A scripts/todo-graph >/dev/null 2>&1
        git commit --quiet --no-verify -m "22bm: restore a real protocol file at head" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_SYMBASE" --head HEAD \
        >"$TMP_DIR/gate-22bm2.log" 2>&1)
    G_RC=$?
    G_BM2_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bm2.log" | head -1)"
    if [ "$G_RC" -eq 3 ] && grep -q 'BASE_PROTOCOL_NOT_A_FILE' "$TMP_DIR/gate-22bm2.log"; then
        t_pass "identity gate: the same contract holds for a BASE whose protocol is a symlink"
    else
        t_fail "identity gate: a symlinked base protocol reached a verdict (rc=$G_RC; got [$G_BM2_TOKEN]; see $TMP_DIR/gate-22bm2.log)"
    fi

    # 22bn: THE LOADER IS PART OF THE DATA FORM. `proto_of`'s data branch
    # validates the JSON by IMPORTING snapshot_protocol.py, so that file is
    # executed on the success path -- and checking only the JSON left the
    # loader free to be a symlink to a byte-identical copy, running while the
    # contract passed. 22bm cannot see this: it symlinks the JSON, which is the
    # file that WAS checked.
    (
        cd "$GATE_REPO" || exit 1
        cp scripts/todo-graph/snapshot_protocol.py scripts/todo-graph/protocol_loader_real.py
        rm -f scripts/todo-graph/snapshot_protocol.py
        ln -s protocol_loader_real.py scripts/todo-graph/snapshot_protocol.py
        printf '\n# 22bn: benign closure change, no behaviour\n' >> scripts/todo-graph/build.py
        git add -A scripts/todo-graph >/dev/null 2>&1
        git commit --quiet --no-verify -m "22bn: the protocol LOADER is a resolvable symlink" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22bn.log" 2>&1)
    G_RC=$?
    G_BN_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bn.log" | head -1)"
    if [ "$G_RC" -eq 3 ] && grep -q 'HEAD_PROTOCOL_NOT_A_FILE' "$TMP_DIR/gate-22bn.log"; then
        t_pass "identity gate: the data form's LOADER must be a regular file too"
    else
        t_fail "identity gate: a symlinked protocol loader reached a verdict (rc=$G_RC; got [$G_BN_TOKEN]; see $TMP_DIR/gate-22bn.log)"
    fi
    G_LOADER_BASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        rm -f scripts/todo-graph/snapshot_protocol.py
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
        git rm -q --ignore-unmatch scripts/todo-graph/protocol_loader_real.py >/dev/null 2>&1
        git add -A scripts/todo-graph >/dev/null 2>&1
        git commit --quiet --no-verify -m "22bn: restore a real loader at head" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_LOADER_BASE" --head HEAD \
        >"$TMP_DIR/gate-22bn2.log" 2>&1)
    G_RC=$?
    G_BN2_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bn2.log" | head -1)"
    if [ "$G_RC" -eq 3 ] && grep -q 'BASE_PROTOCOL_NOT_A_FILE' "$TMP_DIR/gate-22bn2.log"; then
        t_pass "identity gate: the loader contract holds for the BASE side too"
    else
        t_fail "identity gate: a symlinked base loader reached a verdict (rc=$G_RC; got [$G_BN2_TOKEN]; see $TMP_DIR/gate-22bn2.log)"
    fi

    # 22bo: UNTRACKED BYTECODE MUST NOT DECIDE A VERDICT. `proto_of` loads the
    # protocol modules with `spec_from_file_location` + `exec_module`, which
    # execute a timestamp-valid `__pycache__` entry in preference to the source
    # they were handed -- reproduced directly: a spliced cache returned its own
    # value rather than the file's. Untracked bytes deciding what a commit
    # means is the one thing every check here exists to prevent.
    #
    # THE FIRST CUT OF THIS CASE WAS VACUOUS and a reviewer caught it: it
    # exited quietly when no cache was produced (reachable under
    # PYTHONDONTWRITEBYTECODE), and asserted only that one token was ABSENT --
    # so it recorded PASS for rc 1, rc 2, an unrelated rc 3, or no poison at
    # all. It now proves the poison exists, requires an exact gate result, and
    # carries a MUTATION control that must FAIL.
    (
        cd "$GATE_REPO" || exit 1
        printf '\n# 22bo: benign closure change, no behaviour\n' >> scripts/todo-graph/build.py
        git commit --quiet --no-verify -am "22bo: benign closure change" >/dev/null 2>&1
    )
    G_POISON_OK=0
    (
        cd "$GATE_REPO" || exit 1
        unset PYTHONDONTWRITEBYTECODE
        python3 - <<'PY2'
import importlib.util, pathlib, py_compile, sys
src = pathlib.Path("scripts/todo-graph/snapshot_protocol.py")
spec = importlib.util.spec_from_file_location("sp_warm", str(src))
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
cache = sorted(pathlib.Path("scripts/todo-graph/__pycache__").glob("snapshot_protocol.*.pyc"))
if not cache:
    sys.exit(1)
alt = pathlib.Path("scripts/todo-graph/_poison_src.py")
alt.write_text("SNAPSHOT_SCHEMA = 999999\nPRE_RESOLUTION_BUCKETS = ()\n"
               "POST_RESOLUTION_BUCKETS = ()\nALL_BUCKETS = ()\n")
py_compile.compile(str(alt), cfile="/tmp/s50-poison.pyc", dfile=str(src), doraise=True)
good = cache[0].read_bytes()
bad = pathlib.Path("/tmp/s50-poison.pyc").read_bytes()
cache[0].write_bytes(good[:16] + bad[16:])
alt.unlink()
# PROVE THE POISON TOOK: importing the SAME source must now yield the
# poisoned value, or there is nothing for this case to test.
spec2 = importlib.util.spec_from_file_location("sp_check", str(src))
m2 = importlib.util.module_from_spec(spec2)
spec2.loader.exec_module(m2)
sys.exit(0 if getattr(m2, "SNAPSHOT_SCHEMA", None) == 999999 else 1)
PY2
    ) && G_POISON_OK=1
    if [ "$G_POISON_OK" -ne 1 ]; then
        t_fail "identity gate: could not create a poisoned __pycache__ entry, so the bytecode case proves nothing"
    else
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
            >"$TMP_DIR/gate-22bo.log" 2>&1)
        G_RC=$?
        G_BO_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bo.log" | head -1)"
        # AN EXACT RESULT, not the absence of one token: the range is
        # verdict-neutral, so the gate must complete and exit 0.
        if [ "$G_RC" -eq 0 ] && ! grep -q 'PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22bo.log"; then
            t_pass "identity gate: a poisoned __pycache__ entry does not decide the protocol"
        else
            t_fail "identity gate: untracked bytecode changed the verdict (rc=$G_RC; got [$G_BO_TOKEN]; see $TMP_DIR/gate-22bo.log)"
        fi
        # MUTATION: strip the gate-owned cache prefix from the CLONE's own copy
        # and the very same poison must now break the protocol read. Without
        # this the case cannot distinguish "the fix works" from "the poison
        # never mattered".
        # SINGLE-QUOTED sed script: the first cut used double quotes, so the
        # shell expanded `$TMP_DIR` into the pattern before sed ever saw it and
        # the mutation silently matched nothing.
        # TWO DEFENCES NOW, SO THE MUTATION MUST REMOVE BOTH (section 54).
        # Stripping the cache prefix alone stopped firing: the head side reads a
        # worktree the gate materializes for the commit, and that tree has no
        # `__pycache__` for the poison to live in. So the poison is doubly
        # unreachable, and a mutation that removes only the prefix would report
        # "the fix works" while actually measuring the OTHER fix -- the precise
        # vacuity this mutation exists to prevent. Composed, not rewritten: the
        # head-read revert is the same shared helper every inverted case uses.
        G_MUT_PY="$GATE_REPO/scripts/todo-graph/identity-gate-nopycache.sh"
        if ! g_s54_wtgate "$G_MUT_PY"; then
            t_fail "identity gate: could not build the pycache mutation (the head-read shape moved)"
        else
        sed -i 's#^export PYTHONPYCACHEPREFIX=.*#:#' "$G_MUT_PY"
        if grep -q '^export PYTHONPYCACHEPREFIX' "$G_MUT_PY"; then
            t_fail "identity gate: could not build the pycache mutation (the export line moved)"
        else
            (cd "$GATE_REPO" && bash "$G_MUT_PY" --base "$G_PROTO_BASE" --head HEAD \
                >"$TMP_DIR/gate-22bo-mut.log" 2>&1)
            G_RC=$?
            # The poison is at HEAD, so the HEAD token is the one this
            # mutation must produce -- the shared substring would also accept a
            # base-side refusal that has nothing to do with the bytecode.
            if [ "$G_RC" -eq 3 ] \
               && grep -q 'HEAD_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22bo-mut.log"; then
                t_pass "identity gate: MUTATION -- without the gate-owned cache prefix the poison DOES decide"
            else
                t_fail "identity gate: the pycache mutation did not fire, so 22bo proves nothing (rc=$G_RC; see $TMP_DIR/gate-22bo-mut.log)"
            fi
        fi
        fi
        rm -f "$G_MUT_PY"
    fi
    (cd "$GATE_REPO" && rm -rf scripts/todo-graph/__pycache__) >/dev/null 2>&1

    # 22bp: AN ANCESTOR DIRECTORY SYMLINK. `-f` follows symlinks in EVERY path
    # component while `-L` tests only the last one, so symlinking the
    # `scripts/todo-graph` DIRECTORY to a location outside the checkout left
    # every protocol file passing both tests while the gate read and executed
    # bytes from outside the repository.
    G_OUTSIDE="$TMP_DIR/s50-outside-todograph"
    rm -rf "$G_OUTSIDE" 2>/dev/null || true
    if cp -a "$GATE_REPO/scripts/todo-graph" "$G_OUTSIDE" 2>/dev/null; then
        (
            cd "$GATE_REPO" || exit 1
            rm -rf scripts/todo-graph
            ln -s "$G_OUTSIDE" scripts/todo-graph
        )
        # THROUGH THE IN-REPO PATH, not the resolved one. The gate derives its
        # repo root from its own location, so invoking the physical copy makes
        # it operate on a directory that is not the fixture repo at all -- the
        # first cut did exactly that and died before reaching the contract.
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" \
            --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22bp.log" 2>&1)
        G_RC=$?
        G_BP_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bp.log" | head -1)"
        # INVERTED BY SECTION 54 (control at 22as), and inverted in the
        # STRONGEST direction available: the defect this case was written for
        # is not merely detected now, it is unreachable. The gate executes and
        # hashes from its own materialization of the head commit, so bytes
        # outside the checkout cannot enter a verdict at all, whatever the
        # operator's `scripts/todo-graph` happens to point at.
        if [ "$G_RC" -ne 3 ] \
           && grep -q 'HEAD_WORKTREE_DIVERGES' "$TMP_DIR/gate-22bp.log" \
           && ! grep -q 'HEAD_PROTOCOL_NOT_A_FILE' "$TMP_DIR/gate-22bp.log"; then
            t_pass "identity gate: an ancestor-directory symlink cannot reach a verdict about the commit"
        else
            t_fail "identity gate: out-of-checkout bytes still reached the verdict (rc=$G_RC; got [$G_BP_TOKEN]; see $TMP_DIR/gate-22bp.log)"
        fi
        (
            cd "$GATE_REPO" || exit 1
            rm -f scripts/todo-graph
            git checkout --quiet -- scripts/todo-graph >/dev/null 2>&1
        )
        if [ -f "$GATE_IN_CLONE" ]; then
            t_pass "identity gate: CONTROL -- the ancestor-symlink case restores the real directory"
        else
            t_fail "identity gate: the ancestor-symlink case left the fixture clone broken"
        fi
    else
        t_fail "identity gate: could not stage the ancestor-directory symlink fixture"
    fi

    # 22bq: A FAILED PROBE ON THE LOADER IS A REPOSITORY FAILURE, NOT A
    # NON-FILE. The data-form contract collapsed every nonzero result from its
    # loader probe through `|| return 1`, so a git failure emitted
    # BASE_PROTOCOL_NOT_A_FILE and told an operator to fix a symlink that does
    # not exist. Forced with a shim that fails `ls-tree` ONLY for the loader
    # path, leaving the three protocol-path probes and the parse intact.
    G_SHIM4="$TMP_DIR/gitshim4-s50"
    mkdir -p "$G_SHIM4"
    {
        printf '#!/usr/bin/env bash\n'
        printf 'if [ "$1" = "ls-tree" ]; then\n'
        printf '  for a in "$@"; do [ "$a" = "scripts/todo-graph/snapshot_protocol.py" ] && exit 128; done\n'
        printf 'fi\n'
        printf 'exec %s "$@"\n' "$G_REAL_GIT"
    } > "$G_SHIM4/git"
    chmod +x "$G_SHIM4/git"
    (cd "$GATE_REPO" && PATH="$G_SHIM4:$PATH" bash "$GATE_IN_CLONE" \
        --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22bq.log" 2>&1)
    G_RC=$?
    G_BQ_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bq.log" | head -1)"
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'BASE_TREE_UNREADABLE' "$TMP_DIR/gate-22bq.log" \
       && ! grep -q 'BASE_PROTOCOL_NOT_A_FILE' "$TMP_DIR/gate-22bq.log"; then
        t_pass "identity gate: a failed loader probe is a repository failure, not a non-file"
    else
        t_fail "identity gate: a git failure on the loader was reported as a non-file (rc=$G_RC; got [$G_BQ_TOKEN]; see $TMP_DIR/gate-22bq.log)"
    fi

    # 22br: THE LOADER MISSING FROM THE WORKTREE ONLY. The commit carries a
    # complete protocol; the checkout does not. That is an incomplete
    # materialization, and it used to read as HEAD_PROTOCOL_UNREADABLE because
    # the form classification counted an intact legacy pair as "complete" even
    # though `proto_of` takes the data form whenever the JSON exists.
    (
        cd "$GATE_REPO" || exit 1
        printf '\n# 22br: benign closure change, no behaviour\n' >> scripts/todo-graph/build.py
        git commit --quiet --no-verify -am "22br: benign closure change" >/dev/null 2>&1
        rm -f scripts/todo-graph/snapshot_protocol.py
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22br.log" 2>&1)
    G_RC=$?
    G_BR_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22br.log" | head -1)"
    # INVERTED BY SECTION 54 (see 22as for the shared control). The loader is
    # missing from the CHECKOUT only; the gate reads the commit, which carries
    # it, so the range is adjudicated normally and the difference is disclosed.
    # The COMMITTED removal below is the half that still refuses, and it is the
    # half that was ever about the tree being pushed.
    if [ "$G_RC" -ne 3 ] \
       && grep -q 'diverges: scripts/todo-graph/snapshot_protocol.py' "$TMP_DIR/gate-22br.log" \
       && ! grep -qE 'HEAD_PROTOCOL_UNMATERIALIZED|HEAD_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22br.log"; then
        t_pass "identity gate: a loader missing from the WORKTREE only is disclosed, not adjudicated"
    else
        t_fail "identity gate: a checkout-only missing loader still decided the verdict (rc=$G_RC; got [$G_BR_TOKEN]; see $TMP_DIR/gate-22br.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet -- scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
    )
    # AND FROM THE COMMIT: same file, different fault, different token.
    (
        cd "$GATE_REPO" || exit 1
        git rm -q --ignore-unmatch scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22br: the loader is gone from the commit" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22br2.log" 2>&1)
    G_RC=$?
    G_BR2_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22br2.log" | head -1)"
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'HEAD_PROTOCOL_INCOMPLETE' "$TMP_DIR/gate-22br2.log" \
       && ! grep -qE 'HEAD_PROTOCOL_UNREADABLE|HEAD_PROTOCOL_UNMATERIALIZED' \
              "$TMP_DIR/gate-22br2.log"; then
        t_pass "identity gate: a loader missing from the COMMIT is an incomplete protocol, not an unreadable one"
    else
        t_fail "identity gate: a committed loader removal took the wrong branch (rc=$G_RC; got [$G_BR2_TOKEN]; see $TMP_DIR/gate-22br2.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
        git commit --quiet --no-verify -am "22br: restore the loader at head" >/dev/null 2>&1
    )

    # 22bs: IMPORT-TIME OUTPUT MUST NOT BECOME THE PROTOCOL RECORD. `proto_of`
    # captures its own stdout as the record the caller parses, so a module that
    # prints while being imported prepends a line to it -- the caller then reads
    # the noise as the record and dies with a generic malformed-line message
    # carrying NONE of the stable tokens this section publishes. An import-time
    # print is noise, not a protocol defect, so the gate must simply be
    # unaffected by it.
    (
        cd "$GATE_REPO" || exit 1
        printf '\nprint("diagnostic noise from import")\n' >> scripts/todo-graph/snapshot_protocol.py
        printf '\n# 22bs: benign closure change, no behaviour\n' >> scripts/todo-graph/build.py
        git commit --quiet --no-verify -am "22bs: the protocol loader prints on import" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22bs.log" 2>&1)
    G_RC=$?
    G_BS_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bs.log" | head -1)"
    if [ "$G_RC" -eq 0 ] && ! grep -q 'protocol line is malformed' "$TMP_DIR/gate-22bs.log"; then
        t_pass "identity gate: an import-time print does not become the protocol record"
    else
        t_fail "identity gate: import noise broke the protocol read (rc=$G_RC; got [$G_BS_TOKEN]; see $TMP_DIR/gate-22bs.log)"
    fi
    # MUTATION: strip the redirect from the CLONE's own copy and the SAME print
    # must break the read, or this case cannot tell a working fix from a print
    # that never mattered.
    G_MUT_OUT="$GATE_REPO/scripts/todo-graph/identity-gate-noredirect.sh"
    sed 's#    with contextlib.redirect_stdout(sys.stderr):#    if True:#' \
        "$GATE_IN_CLONE" > "$G_MUT_OUT"
    # THE CODE LINE, not any mention of it: a comment elsewhere in the file
    # names `redirect_stdout` too, so a bare grep reported the mutation as
    # unbuilt even after sed had removed the statement.
    if grep -q 'with contextlib.redirect_stdout' "$G_MUT_OUT"; then
        t_fail "identity gate: could not build the stdout-redirect mutation (the shape moved)"
    else
        (cd "$GATE_REPO" && bash "$G_MUT_OUT" --base "$G_PROTO_BASE" --head HEAD \
            >"$TMP_DIR/gate-22bs-mut.log" 2>&1)
        G_RC=$?
        if [ "$G_RC" -ne 0 ]; then
            t_pass "identity gate: MUTATION -- without the redirect the same print DOES break the read"
        else
            t_fail "identity gate: the redirect mutation did not fire, so 22bs proves nothing (rc=$G_RC; see $TMP_DIR/gate-22bs-mut.log)"
        fi
    fi
    rm -f "$G_MUT_OUT"

    # 22bt: A WRITE STRAIGHT TO FD 1 GOES AROUND THE REDIRECT, so the caller
    # must refuse a multi-line record on its own -- and refuse it with a NAMED
    # token rather than the generic malformed-line message.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
        printf '\nimport os as _os\n_os.write(1, b"raw fd1 contamination\\n")\n' \
            >> scripts/todo-graph/snapshot_protocol.py
        git commit --quiet --no-verify -am "22bt: the loader writes to fd 1 on import" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22bt.log" 2>&1)
    G_RC=$?
    G_BT_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bt.log" | head -1)"
    # THE ASSERTION MOVED WITH THE DESIGN, and that is the point rather than a
    # concession. This case originally required contamination to be MAPPED to a
    # named token, which was the best available answer while the record shared
    # stdout with the tree. Round 18 removed the channel instead, so there is
    # nothing left to map: a newline-terminated write is now as harmless as an
    # unterminated one, and the gate simply completes. Requiring a refusal here
    # would be preserving a symptom of a design that no longer exists.
    if [ "$G_RC" -eq 0 ] \
       && ! grep -qE 'PROTOCOL_NOT_A_FILE|protocol line is malformed' \
              "$TMP_DIR/gate-22bt.log"; then
        t_pass "identity gate: a newline-terminated fd-1 write cannot reach the record either"
    else
        t_fail "identity gate: fd-1 contamination still affected the verdict (rc=$G_RC; got [$G_BT_TOKEN]; see $TMP_DIR/gate-22bt.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
        git commit --quiet --no-verify -am "22bt: restore a quiet loader at head" >/dev/null 2>&1
    )

    # 22bu: THE UNTERMINATED fd-1 WRITE, which is the variant that actually
    # broke the contract. A newline-terminated write made the capture
    # multi-line and was caught; a write with NO newline concatenated with the
    # record into a single line, shifting every field so the source word became
    # a digest and the gate emitted a confident, wrong token about files that
    # were all perfectly regular. Including a `|` makes the shift maximally
    # plausible. The record now travels on its own channel, so there is no
    # longer a way for the tree to reach it at all.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
        printf '\nimport os as _os\n_os.write(1, b"noise|")\n' \
            >> scripts/todo-graph/snapshot_protocol.py
        printf '\n# 22bu: benign closure change, no behaviour\n' >> scripts/todo-graph/build.py
        git commit --quiet --no-verify -am "22bu: the loader writes an unterminated fd-1 record" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22bu.log" 2>&1)
    G_RC=$?
    G_BU_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bu.log" | head -1)"
    # The record is unreachable from the tree now, so the gate simply completes:
    # what must NOT happen is a field-shifted verdict blaming the files.
    if [ "$G_RC" -eq 0 ] \
       && ! grep -qE 'PROTOCOL_NOT_A_FILE|protocol line is malformed' \
              "$TMP_DIR/gate-22bu.log"; then
        t_pass "identity gate: an unterminated fd-1 write cannot reach the protocol record"
    else
        t_fail "identity gate: fd-1 noise shifted the record's fields (rc=$G_RC; got [$G_BU_TOKEN]; see $TMP_DIR/gate-22bu.log)"
    fi
    # MUTATION: put the record back on stdout in the CLONE's own copy and the
    # same write must corrupt it, or this case cannot tell a working channel
    # from noise that never mattered.
    G_MUT_CH="$GATE_REPO/scripts/todo-graph/identity-gate-stdoutrec.sh"
    # THE MUTATION TRACKS THE CURRENT SHAPE. It was written against an earlier
    # invocation and silently stopped matching when the record moved to fd 3,
    # producing an UNMUTATED copy that passed -- the assertion below caught it,
    # which is the only reason a stale mutation is survivable.
    sed -e 's#^sys.stdout = os.fdopen(3, "w", encoding="utf-8", buffering=1)$#sys.stdout = sys.__stdout__#' \
        "$GATE_IN_CLONE" > "$G_MUT_CH"
    if grep -q '^sys.stdout = os.fdopen(3,' "$G_MUT_CH"; then
        t_fail "identity gate: could not build the record-channel mutation (the shape moved)"
    else
        (cd "$GATE_REPO" && bash "$G_MUT_CH" --base "$G_PROTO_BASE" --head HEAD \
            >"$TMP_DIR/gate-22bu-mut.log" 2>&1)
        G_RC=$?
        if [ "$G_RC" -ne 0 ]; then
            t_pass "identity gate: MUTATION -- with the record back on stdout the same write DOES corrupt it"
        else
            t_fail "identity gate: the record-channel mutation did not fire, so 22bu proves nothing (rc=$G_RC; see $TMP_DIR/gate-22bu-mut.log)"
        fi
    fi
    rm -f "$G_MUT_CH"
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
        git commit --quiet --no-verify -am "22bu: restore a quiet loader at head" >/dev/null 2>&1
    )

    # 22bv: A LOADER THAT TRIES TO WRITE THE GATE'S OWN ANSWER. While the
    # record's path travelled in argv, the very code being imported knew where
    # the gate's answer lived, and an `atexit` handler could replace a finished
    # record with a valid-looking one -- seven plausible fields naming a source
    # the gate then blamed the files for. The path no longer reaches the
    # subprocess at all, and a late append cannot outrank the first line.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
        # THE argv ROUTE ONLY. A handler that instead GUESSES the descriptor
        # and seeks to its start does succeed, and deliberately is not asserted
        # here: code executing inside the interpreter that must import it can
        # reach any in-process channel, so no framing change closes that. It is
        # filed as section 53 with the containment that actually applies -- this
        # loader is in EXEC_CLOSURE, so a change to it cannot ride along as a
        # data-only migration and forces the differential. What the GATE is
        # responsible for is not handing out the address, and that is what this
        # case pins.
        {
            printf '\nimport atexit as _atexit, sys as _sys\n'
            printf 'def _clobber():\n'
            printf '    try:\n'
            printf '        _p = _sys.argv[2]\n'
            printf '    except IndexError:\n'
            printf '        return\n'
            # SCOPED TO THE RECORD. This loader is imported by the gate's other
            # python phases too, whose argv[2] names THEIR output -- an
            # unscoped write clobbers those instead and the case then fails for
            # a reason that has nothing to do with the boundary under test.
            printf '    if "/proto." not in _p:\n'
            printf '        return\n'
            printf '    open(_p, "w").write("1|a|b|garbage|c|d|e\\n")\n'
            printf '_atexit.register(_clobber)\n'
        } >> scripts/todo-graph/snapshot_protocol.py
        printf '\n# 22bv: benign closure change, no behaviour\n' >> scripts/todo-graph/build.py
        git commit --quiet --no-verify -am "22bv: the loader tries to write the gate's record" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22bv.log" 2>&1)
    G_RC=$?
    G_BV_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bv.log" | head -1)"
    if [ "$G_RC" -eq 0 ] \
       && ! grep -qE 'PROTOCOL_NOT_A_FILE|protocol line is malformed' \
              "$TMP_DIR/gate-22bv.log"; then
        t_pass "identity gate: the record's address is never handed to the code being imported"
    else
        t_fail "identity gate: imported code reached the record (rc=$G_RC; got [$G_BV_TOKEN]; see $TMP_DIR/gate-22bv.log)"
    fi
    # MUTATION: hand the record's path back to the subprocess in the CLONE's
    # own copy, and the SAME loader must corrupt the verdict. Without this the
    # case cannot tell a closed boundary from a loader that never reached.
    G_MUT_ARGV="$GATE_REPO/scripts/todo-graph/identity-gate-argvrec.sh"
    # ONE STABLE ANCHOR, not the invocation line. Targeting the invocation made
    # this mutation go stale twice as the probe's launch shape changed, each
    # time producing an unmutated copy that passed -- the assertion caught it
    # both times, which is the only reason a stale mutation is survivable.
    # Rewriting the scrub line re-exposes the record's path exactly as the old
    # design did, and it can be found from the descriptor the shell opened.
    sed 's#^sys.argv = \[sys.argv\[0\]\]$#sys.argv = [sys.argv[0], sys.argv[1], os.readlink("/proc/self/fd/3")]#' \
        "$GATE_IN_CLONE" > "$G_MUT_ARGV"
    if grep -q '^sys.argv = \[sys.argv\[0\]\]$' "$G_MUT_ARGV"; then
        t_fail "identity gate: could not build the argv-exposure mutation (the shape moved)"
    else
        (cd "$GATE_REPO" && bash "$G_MUT_ARGV" --base "$G_PROTO_BASE" --head HEAD \
            >"$TMP_DIR/gate-22bv-mut.log" 2>&1)
        G_RC=$?
        if [ "$G_RC" -ne 0 ]; then
            t_pass "identity gate: MUTATION -- handed the record path, the same loader DOES corrupt the verdict"
        else
            t_fail "identity gate: the argv-exposure mutation did not fire, so 22bv proves nothing (rc=$G_RC; see $TMP_DIR/gate-22bv-mut.log)"
        fi
    fi
    rm -f "$G_MUT_ARGV"
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
        git commit --quiet --no-verify -am "22bv: restore an honest loader at head" >/dev/null 2>&1
    )

    # 22bw: THE GATE'S OWN PLUMBING FAILING IS NOT A VERDICT ABOUT A TREE.
    # Moving the protocol record between two processes can fail for reasons
    # that say nothing about either side -- no temp file, no interpreter, an
    # empty write, an unreadable record. Those collapsed into UNREADABLE, which
    # labelled a perfectly valid base as malformed, and a failing `head` fell
    # through to the generic un-tokened malformed-line message. Both forced
    # here with PATH shims that break exactly one tool.
    # SCOPED TO THE RECORD FILE. The gate also mktemps its own working
    # DIRECTORY at startup, so a blanket failure kills it before it reaches the
    # transport this case is about -- the first cut did exactly that and died
    # with an un-tokened startup error.
    G_SHIM_MK="$TMP_DIR/gitshim-mktemp-s50"
    mkdir -p "$G_SHIM_MK"
    G_REAL_MKTEMP="$(command -v mktemp)"
    {
        printf '#!/usr/bin/env bash\n'
        printf 'for a in "$@"; do [ "$a" = "-d" ] && exec %s "$@"; done\n' "$G_REAL_MKTEMP"
        printf 'exit 7\n'
    } > "$G_SHIM_MK/mktemp"
    chmod +x "$G_SHIM_MK/mktemp"
    (cd "$GATE_REPO" && PATH="$G_SHIM_MK:$PATH" bash "$GATE_IN_CLONE" \
        --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22bw.log" 2>&1)
    G_RC=$?
    G_BW_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bw.log" | head -1)"
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'BASE_TRANSPORT_FAILED' "$TMP_DIR/gate-22bw.log" \
       && ! grep -qE 'PROTOCOL_UNREADABLE|protocol line is malformed' \
              "$TMP_DIR/gate-22bw.log"; then
        t_pass "identity gate: a record-transport failure is named as machinery, not as a bad protocol"
    else
        t_fail "identity gate: transport failure was blamed on a tree (rc=$G_RC; got [$G_BW_TOKEN]; see $TMP_DIR/gate-22bw.log)"
    fi

    # 22bx: THE READ SIDE OF THE SAME TRANSPORT. A failing `head` had its
    # status ignored entirely, so the caller parsed an empty string and died
    # with the generic malformed-line message carrying no token at all.
    G_SHIM_HD="$TMP_DIR/gitshim-head-s50"
    mkdir -p "$G_SHIM_HD"
    printf '#!/usr/bin/env bash\nexit 9\n' > "$G_SHIM_HD/head"
    chmod +x "$G_SHIM_HD/head"
    (cd "$GATE_REPO" && PATH="$G_SHIM_HD:$PATH" bash "$GATE_IN_CLONE" \
        --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22bx.log" 2>&1)
    G_RC=$?
    G_BX_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bx.log" | head -1)"
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'BASE_TRANSPORT_FAILED' "$TMP_DIR/gate-22bx.log" \
       && ! grep -q 'protocol line is malformed' "$TMP_DIR/gate-22bx.log"; then
        t_pass "identity gate: a failing record READ is named too, not left to the generic message"
    else
        t_fail "identity gate: a failed record read escaped the token contract (rc=$G_RC; got [$G_BX_TOKEN]; see $TMP_DIR/gate-22bx.log)"
    fi

    # 22by: WHICH SIDE ANSWERS WHEN THE BASE IS CLASSIFIABLE AND THE HEAD'S
    # TRANSPORT FAILS. The base decides whether the RANGE can be adjudicated at
    # all, so a base token wins -- but the transport checks were placed beside
    # each other ahead of both classifications, so a HEAD machinery failure
    # outranked a perfectly classifiable BASE one and said nothing about the
    # base. The shim fails the record read only on the SECOND call, which is
    # the head probe.
    G_SHIM_HD2="$TMP_DIR/gitshim-head2-s50"
    mkdir -p "$G_SHIM_HD2"
    G_REAL_HEAD="$(command -v head)"
    G_HD_COUNT="$TMP_DIR/s50-head-calls"
    rm -f "$G_HD_COUNT"
    {
        printf '#!/usr/bin/env bash\n'
        printf 'if [ "$1" = "-n" ] && [ "$2" = "1" ]; then\n'
        printf '  printf x >> "%s"\n' "$G_HD_COUNT"
        printf '  if [ "$(wc -c < "%s")" -ge 2 ]; then exit 9; fi\n' "$G_HD_COUNT"
        printf 'fi\n'
        printf 'exec %s "$@"\n' "$G_REAL_HEAD"
    } > "$G_SHIM_HD2/head"
    chmod +x "$G_SHIM_HD2/head"
    (cd "$GATE_REPO" && PATH="$G_SHIM_HD2:$PATH" bash "$GATE_IN_CLONE" \
        --base "$G_CORRUPT" --head HEAD >"$TMP_DIR/gate-22by.log" 2>&1)
    G_RC=$?
    G_BY_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22by.log" | head -1)"
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'BASE_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22by.log" \
       && ! grep -q 'HEAD_TRANSPORT_FAILED' "$TMP_DIR/gate-22by.log"; then
        t_pass "identity gate: a classifiable BASE still outranks a HEAD transport failure"
    else
        t_fail "identity gate: a head machinery failure outranked the base verdict (rc=$G_RC; got [$G_BY_TOKEN]; see $TMP_DIR/gate-22by.log)"
    fi
    rm -f "$G_HD_COUNT"

    # 22bz: A STALLED HEAD PROBE MUST NOT SILENCE A CLASSIFIABLE BASE. 22by
    # injects an IMMEDIATE second-probe failure, which the old ordering
    # survived by accident; a probe that BLOCKS is the case that exposed it,
    # because the head record was acquired before any base adjudication and
    # `proto_of` was the one phase in the file running unbounded. The base is
    # malformed here, so its token is the correct answer whatever the head does.
    G_SHIM_SLEEP="$TMP_DIR/gitshim-stall-s50"
    mkdir -p "$G_SHIM_SLEEP"
    G_ST_COUNT="$TMP_DIR/s50-stall-calls"
    rm -f "$G_ST_COUNT"
    {
        printf '#!/usr/bin/env bash\n'
        printf 'if [ "$1" = "-n" ] && [ "$2" = "1" ]; then\n'
        printf '  printf x >> "%s"\n' "$G_ST_COUNT"
        printf '  if [ "$(wc -c < "%s")" -ge 2 ]; then sleep 600; fi\n' "$G_ST_COUNT"
        printf 'fi\n'
        printf 'exec %s "$@"\n' "$G_REAL_HEAD"
    } > "$G_SHIM_SLEEP/head"
    chmod +x "$G_SHIM_SLEEP/head"
    G_STALL_START="$(date +%s)"
    (cd "$GATE_REPO" && PATH="$G_SHIM_SLEEP:$PATH" IDENTITY_GATE_BUDGET_SECS=20 \
        bash "$GATE_IN_CLONE" --base "$G_CORRUPT" --head HEAD \
        >"$TMP_DIR/gate-22bz.log" 2>&1)
    G_RC=$?
    G_STALL_ELAPSED=$(( $(date +%s) - G_STALL_START ))
    G_BZ_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22bz.log" | head -1)"
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'BASE_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22bz.log" \
       && [ "$G_STALL_ELAPSED" -lt 120 ]; then
        t_pass "identity gate: a stalled HEAD probe cannot silence a classifiable base (${G_STALL_ELAPSED}s)"
    else
        t_fail "identity gate: a stalled head probe wedged or outranked the base (rc=$G_RC; ${G_STALL_ELAPSED}s; got [$G_BZ_TOKEN]; see $TMP_DIR/gate-22bz.log)"
    fi
    rm -f "$G_ST_COUNT"

    # 22ca: A BASE THAT PARSES BUT VIOLATES THE FILE CONTRACT, WITH THE HEAD
    # PROBE STALLED. 22bz proves the ordering for an UNREADABLE base; this is
    # the SUCCESS-path hole beside it -- the base's regular-file contract ran
    # after the head probe, so a base whose protocol parses through a
    # resolvable symlink reported HEAD_TRANSPORT_FAILED whenever the head probe
    # stalled, and said nothing about the base at all.
    (
        cd "$GATE_REPO" || exit 1
        cp scripts/todo-graph/snapshot_protocol.json scripts/todo-graph/protocol_real2.json
        rm -f scripts/todo-graph/snapshot_protocol.json
        ln -s protocol_real2.json scripts/todo-graph/snapshot_protocol.json
        git add -A scripts/todo-graph >/dev/null 2>&1
        git commit --quiet --no-verify -m "22ca: base protocol reached through a symlink" >/dev/null 2>&1
    )
    G_SYMBASE2="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        rm -f scripts/todo-graph/snapshot_protocol.json
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json >/dev/null 2>&1
        git rm -q --ignore-unmatch scripts/todo-graph/protocol_real2.json >/dev/null 2>&1
        git add -A scripts/todo-graph >/dev/null 2>&1
        git commit --quiet --no-verify -m "22ca: restore a real protocol file at head" >/dev/null 2>&1
    )
    G_ST2_COUNT="$TMP_DIR/s50-stall2-calls"
    rm -f "$G_ST2_COUNT"
    G_SHIM_ST2="$TMP_DIR/gitshim-stall2-s50"
    mkdir -p "$G_SHIM_ST2"
    {
        printf '#!/usr/bin/env bash\n'
        printf 'if [ "$1" = "-n" ] && [ "$2" = "1" ]; then\n'
        printf '  printf x >> "%s"\n' "$G_ST2_COUNT"
        printf '  if [ "$(wc -c < "%s")" -ge 2 ]; then sleep 600; fi\n' "$G_ST2_COUNT"
        printf 'fi\n'
        printf 'exec %s "$@"\n' "$G_REAL_HEAD"
    } > "$G_SHIM_ST2/head"
    chmod +x "$G_SHIM_ST2/head"
    G_CA_START="$(date +%s)"
    (cd "$GATE_REPO" && PATH="$G_SHIM_ST2:$PATH" IDENTITY_GATE_BUDGET_SECS=20 \
        bash "$GATE_IN_CLONE" --base "$G_SYMBASE2" --head HEAD \
        >"$TMP_DIR/gate-22ca.log" 2>&1)
    G_RC=$?
    G_CA_ELAPSED=$(( $(date +%s) - G_CA_START ))
    G_CA_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22ca.log" | head -1)"
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'BASE_PROTOCOL_NOT_A_FILE' "$TMP_DIR/gate-22ca.log" \
       && ! grep -q 'HEAD_TRANSPORT_FAILED' "$TMP_DIR/gate-22ca.log" \
       && [ "$G_CA_ELAPSED" -lt 120 ]; then
        t_pass "identity gate: a parsed base's file contract is settled before the head is asked (${G_CA_ELAPSED}s)"
    else
        t_fail "identity gate: a stalled head outranked the base's file contract (rc=$G_RC; ${G_CA_ELAPSED}s; got [$G_CA_TOKEN]; see $TMP_DIR/gate-22ca.log)"
    fi
    rm -f "$G_ST2_COUNT"

    # 22cb: ONLY THE JSON MISSING FROM THE WORKTREE. 22as removes all four
    # protocol paths and 22br removes only the loader; this removes only the
    # JSON, and the surviving legacy pair then read as a COMPLETE form on its
    # own -- so the tree was called malformed when `proto_of` had failed
    # because a file the commit DOES carry was not there to read. Section 50
    # answered that with a path-by-path comparison against the commit; section
    # 54 removed the question by not reading the worktree at all, so what this
    # case now asserts is that the corrupted checkout is disclosed and
    # otherwise ignored.
    (
        cd "$GATE_REPO" || exit 1
        printf '\n# 22cb: benign closure change, no behaviour\n' >> scripts/todo-graph/build.py
        git commit --quiet --no-verify -am "22cb: benign closure change" >/dev/null 2>&1
        rm -f scripts/todo-graph/snapshot_protocol.json
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22cb.log" 2>&1)
    G_RC=$?
    G_CB_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22cb.log" | head -1)"
    # INVERTED BY SECTION 54 (control at 22as). The surviving-legacy-pair
    # ambiguity this case was written for cannot arise any more: the gate never
    # asks what the worktree can satisfy on its own, because it never reads the
    # worktree. The disclosure is asserted by NAME so the case still proves the
    # file was really missing rather than passing on an unrelated clean run.
    if [ "$G_RC" -ne 3 ] \
       && grep -q 'diverges: scripts/todo-graph/snapshot_protocol.json' "$TMP_DIR/gate-22cb.log" \
       && ! grep -qE 'HEAD_PROTOCOL_UNMATERIALIZED|HEAD_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22cb.log"; then
        t_pass "identity gate: a single protocol file missing from the checkout is disclosed, not adjudicated"
    else
        t_fail "identity gate: a checkout-only missing JSON still decided the verdict (rc=$G_RC; got [$G_CB_TOKEN]; see $TMP_DIR/gate-22cb.log)"
    fi
    # CONTROL: restored, the same range reaches a normal verdict.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet -- scripts/todo-graph/snapshot_protocol.json >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22cb-control.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 0 ]; then
        t_pass "identity gate: CONTROL -- with the file restored the same range is clean"
    else
        t_fail "identity gate: the 22cb control did not reach a clean verdict (rc=$G_RC; see $TMP_DIR/gate-22cb-control.log)"
    fi

    # 22cc: A FAILED HEAD TREE PROBE UNDER A COMPLETE WORKTREE FORM. 22ba
    # removes every worktree protocol path, so its form is `none` and execution
    # reaches a later probe; this keeps the worktree COMPLETE but malformed, so
    # the comparison probe added one round earlier is the FIRST thing to ask
    # git -- and its failure was swallowed by a bare `if`, letting the run go
    # on to blame the tree for a question the repository could not answer.
    (
        cd "$GATE_REPO" || exit 1
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["snapshot_schema"] = "not-an-int"
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22cc: complete but malformed head protocol" >/dev/null 2>&1
    )
    G_HEAD_SHA2="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    G_SHIM5="$TMP_DIR/gitshim5-s50"
    mkdir -p "$G_SHIM5"
    {
        printf '#!/usr/bin/env bash\n'
        printf 'if [ "$1" = "ls-tree" ]; then\n'
        printf '  for a in "$@"; do [ "$a" = "%s" ] && exit 128; done\n' "$G_HEAD_SHA2"
        printf 'fi\n'
        printf 'exec %s "$@"\n' "$G_REAL_GIT"
    } > "$G_SHIM5/git"
    chmod +x "$G_SHIM5/git"
    (cd "$GATE_REPO" && PATH="$G_SHIM5:$PATH" bash "$GATE_IN_CLONE" \
        --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22cc.log" 2>&1)
    G_RC=$?
    G_CC_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22cc.log" | head -1)"
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'HEAD_TREE_UNREADABLE' "$TMP_DIR/gate-22cc.log" \
       && ! grep -q 'HEAD_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22cc.log"; then
        t_pass "identity gate: a failed head tree probe is named even when the worktree form is complete"
    else
        t_fail "identity gate: a swallowed probe failure blamed the tree (rc=$G_RC; got [$G_CC_TOKEN]; see $TMP_DIR/gate-22cc.log)"
    fi
    # CONTROL: without the shim the same state is a malformed protocol, which
    # is what proves the shim is doing the work rather than the malformed JSON.
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22cc-control.log" 2>&1)
    if grep -q 'HEAD_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22cc-control.log"; then
        t_pass "identity gate: CONTROL -- without the shim the same head reads as malformed"
    else
        t_fail "identity gate: the 22cc control did not reach the malformed verdict (see $TMP_DIR/gate-22cc-control.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json >/dev/null 2>&1
        git commit --quiet --no-verify -am "22cc: restore a well-formed protocol at head" >/dev/null 2>&1
    )

    # THE CLONE MUST LEAVE THIS CASE AS IT ENTERED IT. Four later cases were
    # lost to a worktree this block emptied; assert the recovery rather than
    # trusting it.
    if [ -f "$GATE_IN_CLONE" ] && [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
        t_pass "identity gate: the replacement-ref case leaves the fixture clone intact"
    else
        t_fail "identity gate: the replacement-ref case damaged the shared fixture clone"
    fi

    # 22cd: THE BYTE-IDENTICAL CLOSURE EXIT ANSWERS TO THE PROTOCOL (section
    # 51). Every other protocol case in this block reaches the classification
    # because SOMETHING in the closure changed; this one is the opposite shape
    # and the only one that used to reach rc 0. Base and head share one
    # unloadable snapshot_protocol.json and differ only in a `todo/` file, so
    # the closure is byte-identical and the exit fired before a protocol byte
    # had been read -- returning 0 with no token at all, and making that head
    # the next gated baseline. Section 18 had already refused the same shape at
    # the schema fast path, so the two entry points disagreed.
    (
        cd "$GATE_REPO" || exit 1
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["snapshot_schema"] = "not-an-int"
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
        git commit --quiet --no-verify -am "22cd: an unloadable protocol shared by base and head" >/dev/null 2>&1
    )
    G_CD_BASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        t="$(find todo -name 'TODO-*.md' | sort | head -1)"
        printf '\n- [ ] section 51 fixture: a todo-only edit over an unloadable protocol\n' >>"$t"
        git commit --quiet --no-verify -am "22cd: todo-only edit, closure untouched" >/dev/null 2>&1
    )
    # ASSERT THE CLOSURE REALLY IS IDENTICAL, or the case proves nothing: a
    # stray resolver edit would send it down the ordinary path and it would
    # pass against the very gate this fixture exists to catch.
    G_CD_CLOSURE_DIFF="$( (cd "$GATE_REPO" && git diff --name-only "$G_CD_BASE" HEAD -- scripts/todo-graph scripts/lint/check_bucket_emission.py) )"
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CD_BASE" --head HEAD \
        >"$TMP_DIR/gate-22cd.log" 2>&1)
    G_RC=$?
    G_CD_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22cd.log" | head -1)"
    if [ -z "$G_CD_CLOSURE_DIFF" ] && [ "$G_RC" -eq 3 ] \
       && grep -qE 'BASE_PROTOCOL_UNREADABLE|HEAD_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22cd.log"; then
        t_pass "identity gate: a byte-identical closure over an unloadable protocol is named, not exited 0"
    else
        t_fail "identity gate: the closure exit outranked the protocol (rc=$G_RC; got [$G_CD_TOKEN]; closure diff [$G_CD_CLOSURE_DIFF]; see $TMP_DIR/gate-22cd.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/snapshot_protocol.json >/dev/null 2>&1
        git commit --quiet --no-verify -am "22cd: restore a well-formed protocol" >/dev/null 2>&1
    )
    # CONTROL: the exit itself is not what was removed. The SAME shape over a
    # well-formed protocol -- byte-identical closure, todo-only diff -- must
    # still be a silent rc 0, or this change has turned an ordinary roadmap
    # commit into a gate failure.
    G_CD_OK_BASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        t="$(find todo -name 'TODO-*.md' | sort | head -1)"
        printf '\n- [ ] section 51 fixture: a todo-only edit over a well-formed protocol\n' >>"$t"
        git commit --quiet --no-verify -am "22cd: todo-only edit over a healthy protocol" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CD_OK_BASE" --head HEAD \
        >"$TMP_DIR/gate-22cd-control.log" 2>&1)
    G_RC=$?
    # THE MARKER, NOT MERELY THE STATUS. rc 0 alone cannot tell "the exit moved"
    # from "the exit was deleted": a healthy byte-identical range that falls all
    # the way through the differential also returns 0, so a deletion would leave
    # this control green while costing every such range a full double walk
    # (Codex test-coverage, section 51, [medium]).
    if [ "$G_RC" -eq 0 ] \
       && grep -q 'resolver closure byte-identical base\.\.head' "$TMP_DIR/gate-22cd-control.log"; then
        t_pass "identity gate: CONTROL -- a healthy byte-identical closure still exits 0 THROUGH the exit"
    else
        t_fail "identity gate: the byte-identical exit stopped working or was bypassed (rc=$G_RC; see $TMP_DIR/gate-22cd-control.log)"
    fi

    # 22ce: THE SAME ORDERING QUESTION, ASKED THROUGH THE FILE CONTRACT RATHER
    # THAN THE PARSE (section 51). 22bp proves an ancestor-directory symlink is
    # refused -- but it reaches that refusal down the ordinary path, because
    # its range changes the closure. Here the closure is byte-identical
    # (`git hash-object` reads THROUGH the symlink, so the external copy hashes
    # the same) and the diff is one `todo/` file, which is precisely the shape
    # a record-shaped predicate beside the old exit would have waved through:
    # `proto_of` follows the symlink and returns a clean seven-field record,
    # and only `protocol_source_is_regular` -- which runs after it -- refuses.
    G_CE_OUTSIDE="$TMP_DIR/s51-outside-todograph"
    rm -rf "$G_CE_OUTSIDE" 2>/dev/null || true
    G_CE_BASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        t="$(find todo -name 'TODO-*.md' | sort | head -1)"
        printf '\n- [ ] section 51 fixture: a todo-only edit read through an ancestor symlink\n' >>"$t"
        git commit --quiet --no-verify -am "22ce: todo-only edit, closure untouched" >/dev/null 2>&1
    )
    if cp -a "$GATE_REPO/scripts/todo-graph" "$G_CE_OUTSIDE" 2>/dev/null; then
        (
            cd "$GATE_REPO" || exit 1
            rm -rf scripts/todo-graph
            ln -s "$G_CE_OUTSIDE" scripts/todo-graph
        )
        # Through the IN-REPO path, as 22bp records: invoking the physical copy
        # makes the gate derive a repo root that is not the fixture repo.
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CE_BASE" --head HEAD \
            >"$TMP_DIR/gate-22ce.log" 2>&1)
        G_RC=$?
        G_CE_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22ce.log" | head -1)"
        # INVERTED BY SECTION 54 (control at 22as). The ordering question this
        # case asked -- does the file contract get its say before the
        # byte-identical exit -- is now moot on the head side: the exit is
        # decided from the commit's own blobs, so a symlink in the checkout
        # cannot make an out-of-tree file the thing that was measured.
        if [ "$G_RC" -ne 3 ] \
           && grep -q 'HEAD_WORKTREE_DIVERGES' "$TMP_DIR/gate-22ce.log" \
           && ! grep -q 'HEAD_PROTOCOL_NOT_A_FILE' "$TMP_DIR/gate-22ce.log"; then
            t_pass "identity gate: an out-of-tree protocol cannot ride the byte-identical exit it never enters"
        else
            t_fail "identity gate: an out-of-tree protocol still reached the exit decision (rc=$G_RC; got [$G_CE_TOKEN]; see $TMP_DIR/gate-22ce.log)"
        fi
        (
            cd "$GATE_REPO" || exit 1
            rm -f scripts/todo-graph
            git checkout --quiet -- scripts/todo-graph >/dev/null 2>&1
        )
        rm -rf "$G_CE_OUTSIDE" 2>/dev/null || true
        if [ -f "$GATE_IN_CLONE" ]; then
            t_pass "identity gate: CONTROL -- the section 51 symlink case restores the real directory"
        else
            t_fail "identity gate: the section 51 symlink case left the fixture clone broken"
        fi
    else
        t_fail "identity gate: could not stage the section 51 ancestor-symlink fixture"
    fi

    # 22cf: THE MEASUREMENT MUST STILL BE TRUE WHEN IT IS ACTED ON (section 51,
    # inverted by section 54). Moving the decision widened the gap between
    # hashing the closure and exiting on it from nothing to a base worktree plus
    # two protocol probes, and while the head side hashed the WORKING TREE a
    # resolver file saved into a shared local checkout inside that window was
    # measured in its old form and rode the exit, making an UNEXAMINED change
    # the next baseline. The head side now reads an immutable commit, so the
    # window is gone rather than narrowed. The shim is kept exactly as it was --
    # it mutates a closure file immediately after `git worktree add`, inside the
    # old window by construction -- because that is what makes the new
    # assertion, that the mutation reaches nothing, worth anything.
    G_CF_BASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        t="$(find todo -name 'TODO-*.md' | sort | head -1)"
        printf '\n- [ ] section 51 fixture: a todo-only edit mutated under the gate\n' >>"$t"
        git commit --quiet --no-verify -am "22cf: todo-only edit, closure untouched" >/dev/null 2>&1
    )
    G_SHIM_S51="$TMP_DIR/gitshim-s51"
    mkdir -p "$G_SHIM_S51"
    {
        printf '#!/usr/bin/env bash\n'
        # ANY ARGUMENT, NOT $1. The gate passes global options before the
        # subcommand since section 54, so a shim keyed on $1 silently stops
        # firing the moment one is added -- and a shim that never fires makes
        # its case pass or fail for a reason unrelated to the code under test.
        # THE TRIGGER FOLLOWS THE MATERIALIZATION, WHICH MOVED (section 55).
        # This fired on `git worktree`, and a byte-identical range no longer
        # runs one at all -- the head side is assembled from the commit's blobs
        # with `ls-tree -r -z`. A shim that never fires makes its case pass or
        # fail for a reason unrelated to the code under test, which is the same
        # hazard the $1-keying note below records. Both shapes are kept, so the
        # case forces its mutation on the fast path AND on a range that checks
        # out.
        printf '_is_wt=0; for a in "$@"; do [ "$a" = "worktree" ] && _is_wt=1; done\n'
        printf '_is_asm=0; _saw_ls=0; _saw_r=0; _saw_z=0\n'
        printf 'for a in "$@"; do\n'
        printf '  [ "$a" = "ls-tree" ] && _saw_ls=1\n'
        printf '  [ "$a" = "-r" ] && _saw_r=1\n'
        printf '  [ "$a" = "-z" ] && _saw_z=1\n'
        printf 'done\n'
        printf '[ "$_saw_ls$_saw_r$_saw_z" = "111" ] && _is_asm=1\n'
        printf 'if [ "$_is_wt" = "1" ] || [ "$_is_asm" = "1" ]; then\n'
        printf '  %s "$@"; _rc=$?\n' "$G_REAL_GIT"
        printf '  printf "\\n# 22cf: mutated under the gate\\n" >> %s\n' \
            "$GATE_REPO/scripts/todo-graph/ref_resolution.py"
        printf '  exit $_rc\n'
        printf 'fi\n'
        printf 'exec %s "$@"\n' "$G_REAL_GIT"
    } > "$G_SHIM_S51/git"
    chmod +x "$G_SHIM_S51/git"
    (cd "$GATE_REPO" && PATH="$G_SHIM_S51:$PATH" bash "$GATE_IN_CLONE" \
        --base "$G_CF_BASE" --head HEAD >"$TMP_DIR/gate-22cf.log" 2>&1)
    G_RC=$?
    # rc 3, NOT a fall-through to the walk: the protocol constants were read
    # before the movement, so walking would adjudicate mixed-time evidence and
    # could return a real-looking verdict about a tree that never existed.
    # INVERTED BY SECTION 54, and this is the case that records what that
    # section actually bought. Section 51 narrowed the measure-to-act window and
    # said in its own text that only adjudicating an immutable snapshot would
    # remove it. That is what the head side now reads, so a resolver file
    # written into the checkout MID-RUN -- the exact race this fixture forces
    # deterministically -- can no longer be measured at all, in either form.
    # The refusal it used to require was the best available answer to a window
    # that no longer exists; requiring it now would be requiring the gate to
    # notice something it is right to be blind to.
    if [ "$G_RC" -ne 3 ] \
       && grep -q 'HEAD_WORKTREE_DIVERGES' "$TMP_DIR/gate-22cf.log" \
       && ! grep -q 'CLOSURE_MOVED_UNDER_GATE' "$TMP_DIR/gate-22cf.log"; then
        t_pass "identity gate: a closure file written under the gate cannot move an immutable head"
    else
        t_fail "identity gate: a mid-run worktree mutation still reached the head measurement (rc=$G_RC; see $TMP_DIR/gate-22cf.log)"
    fi
    (cd "$GATE_REPO" && git checkout --quiet -- scripts/todo-graph/ref_resolution.py >/dev/null 2>&1)

    # 22cj: AN UNANSWERABLE PROBE IS NOT CONFIRMED ABSENCE. The measurement far
    # above collapses every git failure to the string MISSING on BOTH sides, so
    # a closure member that is genuinely absent at the base and merely
    # UNREADABLE at head compares EQUAL -- and an unexamined change rides the
    # exit (Codex adversarial, section 51 round 2, [high]). Both halves are
    # required to reach it: a member present at the base would differ from the
    # unreadable head in the first pass and force a walk, so the fixture must
    # delete it at the base and make the worktree path a DIRECTORY, which
    # `hash-object` cannot read.
    G_CJ_PRE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        git rm --quiet -f scripts/todo-graph/producer_differential.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22cj: closure member absent at the base" >/dev/null 2>&1
    )
    G_CJ_BASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        t="$(find todo -name 'TODO-*.md' | sort | head -1)"
        printf '\n- [ ] section 51 fixture: a todo-only edit with an unreadable closure member\n' >>"$t"
        git commit --quiet --no-verify -am "22cj: todo-only edit, closure untouched" >/dev/null 2>&1
        mkdir -p scripts/todo-graph/producer_differential.py
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CJ_BASE" --head HEAD \
        >"$TMP_DIR/gate-22cj.log" 2>&1)
    G_RC=$?
    G_CJ_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22cj.log" | head -1)"
    # INVERTED BY SECTION 54 (control at 22as). The MISSING-collapse this case
    # exploits is a property of hashing a FILESYSTEM path that cannot be read.
    # The head side now asks the commit for a blob OID, and a commit either
    # carries a member or does not -- there is no unreadable third state for a
    # directory-shaped worktree path to occupy. The base side still hashes
    # through git objects and is untouched, which is why the collapse itself is
    # left exactly as section 51 documented it.
    # NO DIVERGENCE IS DISCLOSED HERE, AND THAT IS CORRECT rather than a gap:
    # the member is absent from both endpoints and the worktree holds an EMPTY
    # DIRECTORY, which git does not track at all. There is nothing about this
    # checkout that differs from the commit in git's own terms, so the honest
    # outcome is the byte-identical exit -- which this case asserts by name, so
    # it cannot pass on a run that died early instead.
    G_CJ_DIR=no; [ -d "$GATE_REPO/scripts/todo-graph/producer_differential.py" ] && G_CJ_DIR=yes
    if [ "$G_CJ_DIR" = yes ] && [ "$G_RC" -eq 0 ] \
       && grep -q 'resolver closure byte-identical base\.\.head' "$TMP_DIR/gate-22cj.log" \
       && ! grep -q 'CLOSURE_UNVERIFIABLE' "$TMP_DIR/gate-22cj.log"; then
        t_pass "identity gate: an unreadable WORKTREE path is not a state the commit can be in"
    else
        t_fail "identity gate: an unreadable worktree path still reached the head measurement (rc=$G_RC; got [$G_CJ_TOKEN]; dir-staged=$G_CJ_DIR; see $TMP_DIR/gate-22cj.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        rm -rf scripts/todo-graph/producer_differential.py
        git checkout --quiet "$G_CJ_PRE" -- scripts/todo-graph/producer_differential.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22cj: restore the closure member" >/dev/null 2>&1
    )

    # 22ck: A BLOB OID SAYS WHAT AN ENTRY CONTAINS, NOT WHAT IT IS. Git stores a
    # symlink as mode 120000 whose blob is the target PATH TEXT, so a base
    # symlink naming `pass` and a head regular file whose whole content is
    # `pass` carry the SAME OID. Presence-only probing therefore called them
    # byte-identical while the base would EXECUTE the symlink target and the
    # head executes those five bytes (Codex adversarial, section 51 round 3,
    # [high]). Nothing about this needs a broken repository: both trees are
    # well-formed and the OIDs genuinely match.
    G_CK_PRE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        rm -f scripts/todo-graph/producer_differential.py
        ln -s pass scripts/todo-graph/producer_differential.py
        git add scripts/todo-graph/producer_differential.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22ck: the closure member is a symlink at the base" >/dev/null 2>&1
    )
    G_CK_BASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        rm -f scripts/todo-graph/producer_differential.py
        printf 'pass' > scripts/todo-graph/producer_differential.py
        t="$(find todo -name 'TODO-*.md' | sort | head -1)"
        printf '\n- [ ] section 51 fixture: a todo-only edit beside a symlink-to-regular swap\n' >>"$t"
        git commit --quiet --no-verify -am "22ck: a regular file whose bytes are the old link target" >/dev/null 2>&1
    )
    # PROVE THE PREMISE FIRST, or a passing assertion means nothing: the two
    # sides must really carry the same OID, and the base entry must really be a
    # symlink. Without both, this case degenerates into an ordinary changed
    # closure and would pass against the very gate it exists to catch.
    G_CK_BOID="$( (cd "$GATE_REPO" && git rev-parse --quiet --verify "$G_CK_BASE:scripts/todo-graph/producer_differential.py" 2>/dev/null) )"
    G_CK_HOID="$( (cd "$GATE_REPO" && git hash-object scripts/todo-graph/producer_differential.py 2>/dev/null) )"
    G_CK_MODE="$( (cd "$GATE_REPO" && git ls-tree "$G_CK_BASE" -- scripts/todo-graph/producer_differential.py 2>/dev/null | cut -d' ' -f1) )"
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CK_BASE" --head HEAD \
        >"$TMP_DIR/gate-22ck.log" 2>&1)
    G_RC=$?
    G_CK_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22ck.log" | head -1)"
    if [ -n "$G_CK_BOID" ] && [ "$G_CK_BOID" = "$G_CK_HOID" ] && [ "$G_CK_MODE" = "120000" ] \
       && [ "$G_RC" -eq 3 ] \
       && grep -q 'CLOSURE_UNVERIFIABLE' "$TMP_DIR/gate-22ck.log" \
       && ! grep -q 'resolver closure byte-identical base\.\.head' "$TMP_DIR/gate-22ck.log"; then
        t_pass "identity gate: an equal-OID symlink-to-regular swap is refused, not called identical"
    else
        t_fail "identity gate: a symlink-to-regular swap rode the byte-identical exit (rc=$G_RC; got [$G_CK_TOKEN]; mode=$G_CK_MODE base=$G_CK_BOID head=$G_CK_HOID; see $TMP_DIR/gate-22ck.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        rm -f scripts/todo-graph/producer_differential.py
        git checkout --quiet "$G_CK_PRE" -- scripts/todo-graph/producer_differential.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22ck: restore the closure member" >/dev/null 2>&1
    )

    # 22cl: THE EXECUTABLE BIT DECIDES WHETHER THE LOCAL GATE RUNS AT ALL, so a
    # mode-only change to identity-gate.sh IS verdict-affecting even though the
    # bytes are identical and every closure member is invoked through an
    # explicit interpreter. `.githooks/pre-push` enters its identity-gate block
    # only when this script is `-x`, so 100755 -> 100644 riding along with a
    # real closure change skips the whole local gate and leaves CI to notice
    # after main has moved (Codex adversarial, section 51 round 4, [high] --
    # which refuted the reasoning that had rejected this comparison one round
    # earlier).
    G_CL_PRE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        # THROUGH THE INDEX, not `chmod`. A fixture that changes the mode on
        # disk and commits it only works where the filesystem carries the bit,
        # which is the very assumption round 5 showed was unsafe to build on.
        git update-index --chmod=-x scripts/todo-graph/identity-gate.sh >/dev/null 2>&1
        t="$(find todo -name 'TODO-*.md' | sort | head -1)"
        printf '\n- [ ] section 51 fixture: a todo-only edit beside a mode-only closure change\n' >>"$t"
        # EXPLICIT PATHS, never `commit -a`. `-a` re-reads the worktree and
        # resets the index entry's mode from the file on disk, silently undoing
        # the `--chmod` this case exists to stage -- the fixture then measured
        # an ordinary todo-only commit and its own premise check caught it.
        git add "$t" >/dev/null 2>&1
        git commit --quiet --no-verify -m "22cl: drop the gate's executable bit, bytes unchanged" >/dev/null 2>&1
    )
    # PREMISE: the CONTENT really is unchanged, so a failure here can only be
    # about the mode.
    G_CL_DIFF="$( (cd "$GATE_REPO" && git diff --name-only "$G_CL_PRE" HEAD -- scripts/todo-graph scripts/lint/check_bucket_emission.py) )"
    G_CL_OLDMODE="$( (cd "$GATE_REPO" && git ls-tree "$G_CL_PRE" -- scripts/todo-graph/identity-gate.sh 2>/dev/null | cut -d' ' -f1) )"
    G_CL_NEWMODE="$( (cd "$GATE_REPO" && git ls-tree HEAD -- scripts/todo-graph/identity-gate.sh 2>/dev/null | cut -d' ' -f1) )"
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CL_PRE" --head HEAD \
        >"$TMP_DIR/gate-22cl.log" 2>&1)
    G_RC=$?
    G_CL_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22cl.log" | head -1)"
    G_CL_OLDOID="$( (cd "$GATE_REPO" && git rev-parse --quiet --verify "$G_CL_PRE:scripts/todo-graph/identity-gate.sh" 2>/dev/null) )"
    G_CL_NEWOID="$( (cd "$GATE_REPO" && git rev-parse --quiet --verify "HEAD:scripts/todo-graph/identity-gate.sh" 2>/dev/null) )"
    # THE BYTE-EQUALITY PREMISE IS TESTED, not merely computed. `G_CL_DIFF` was
    # printed in the failure message and never asserted, so a content change
    # riding along with the mode flip would have produced the expected token and
    # passed a fixture whose whole claim is "mode only" (Codex adversarial,
    # section 51 round 6, [medium]). The premise is BLOB EQUALITY, not an empty
    # `git diff --name-only`: that command lists a path whose MODE changed, so
    # requiring it empty asserts the absence of the very thing being staged.
    if [ "$G_CL_OLDMODE" = "100755" ] && [ "$G_CL_NEWMODE" = "100644" ] \
       && [ -n "$G_CL_OLDOID" ] && [ "$G_CL_OLDOID" = "$G_CL_NEWOID" ] \
       && [ "$G_RC" -eq 3 ] \
       && grep -q 'GATE_SCRIPT_NOT_EXECUTABLE' "$TMP_DIR/gate-22cl.log" \
       && ! grep -q 'resolver closure byte-identical base\.\.head' "$TMP_DIR/gate-22cl.log"; then
        t_pass "identity gate: a mode-only closure change is not byte-identical"
    else
        t_fail "identity gate: a mode-only closure change rode the exit (rc=$G_RC; got [$G_CL_TOKEN]; mode $G_CL_OLDMODE -> $G_CL_NEWMODE; oid $G_CL_OLDOID vs $G_CL_NEWOID; content diff [$G_CL_DIFF]; see $TMP_DIR/gate-22cl.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        git update-index --chmod=+x scripts/todo-graph/identity-gate.sh >/dev/null 2>&1
        git commit --quiet --no-verify -m "22cl: restore the gate's executable bit" >/dev/null 2>&1
        chmod +x scripts/todo-graph/identity-gate.sh
    )
    G_CL_MODE="$( (cd "$GATE_REPO" && git ls-tree HEAD -- scripts/todo-graph/identity-gate.sh 2>/dev/null | cut -d' ' -f1) )"
    # THE RECOVERY LEG, and it is the point of asking this as an invariant. A
    # base-vs-head mode COMPARISON was tried first and had no exit: the base is
    # the last SUCCESSFULLY gated SHA, so it stays pre-change forever and every
    # later head refuses. Restoring the bit must CLEAR the refusal against the
    # unchanged base, or the gate is a trap rather than a check.
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CL_PRE" --head HEAD \
        >"$TMP_DIR/gate-22cl-recover.log" 2>&1)
    G_RC=$?
    if [ "$G_CL_MODE" = "100755" ] && [ "$G_RC" -eq 0 ] \
       && ! grep -q 'GATE_SCRIPT_NOT_EXECUTABLE' "$TMP_DIR/gate-22cl-recover.log"; then
        t_pass "identity gate: restoring the executable bit CLEARS the refusal (recovery path exists)"
    else
        t_fail "identity gate: the mode refusal has no recovery path (rc=$G_RC; mode $G_CL_MODE; see $TMP_DIR/gate-22cl-recover.log)"
    fi

    # 22cm: THE MODE QUESTION IS ASKED OF GIT, NOT OF THE FILESYSTEM, and this
    # is the control that proves it. Git supports checkouts whose filesystem
    # does not carry the executable bit (`core.fileMode=false`, and mounts that
    # report every file executable); an implementation that read `[ -x ]` for
    # the head side compared 100644 against 100755 on a PERFECTLY HEALTHY range
    # there and refused at rc 3, wedging every resolver push on that host
    # (Codex adversarial, section 51 round 5, [high]). It passed everywhere here
    # because this checkout has core.fileMode=true, which is exactly why no
    # mode-preserving fixture could have found it.
    G_CM_BASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    # CAPTURE THE ORIGINAL, do not assume it. Teardown used to set the value to
    # `true` unconditionally, which is a restore only on a mode-preserving host;
    # on a genuinely mode-blind filesystem the clone starts `false` and every
    # later shared fixture would inherit a setting this case invented (Codex
    # adversarial, section 51 round 6, [medium]).
    G_CM_FM_WAS="$( (cd "$GATE_REPO" && git config --get core.fileMode 2>/dev/null) )"
    (
        cd "$GATE_REPO" || exit 1
        git config core.fileMode false || true
        # Both directions of the disagreement, on a tree git considers clean:
        # the gate loses its bit on disk while the commit keeps it, and a
        # non-executable member gains one.
        chmod -x scripts/todo-graph/identity-gate.sh
        chmod +x scripts/todo-graph/snapshot_protocol.json
        t="$(find todo -name 'TODO-*.md' | sort | head -1)"
        printf '\n- [ ] section 51 fixture: a todo-only edit on a mode-blind checkout\n' >>"$t"
        git commit --quiet --no-verify -am "22cm: todo-only edit, closure untouched" >/dev/null 2>&1
    )
    # PREMISE: git must consider the worktree CLEAN despite the on-disk change,
    # or the case is testing a dirty tree instead of a mode-blind one.
    G_CM_DIRTY="$( (cd "$GATE_REPO" && git status --porcelain -- scripts/todo-graph) )"
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CM_BASE" --head HEAD \
        >"$TMP_DIR/gate-22cm.log" 2>&1)
    G_RC=$?
    if [ -z "$G_CM_DIRTY" ] && [ "$G_RC" -eq 0 ] \
       && grep -q 'resolver closure byte-identical base\.\.head' "$TMP_DIR/gate-22cm.log" \
       && ! grep -q 'CLOSURE_MOVED_UNDER_GATE' "$TMP_DIR/gate-22cm.log"; then
        t_pass "identity gate: CONTROL -- a mode-blind checkout still exits 0 on a healthy range"
    else
        t_fail "identity gate: a core.fileMode=false checkout was refused (rc=$G_RC; dirty [$G_CM_DIRTY]; see $TMP_DIR/gate-22cm.log)"
    fi
    # And the committed flip is STILL refused under the same configuration,
    # which is what keeps this a portability control rather than an escape.
    (
        cd "$GATE_REPO" || exit 1
        git update-index --chmod=-x scripts/todo-graph/identity-gate.sh >/dev/null 2>&1
        git commit --quiet --no-verify -m "22cm: committed mode flip on a mode-blind checkout" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CM_BASE" --head HEAD \
        >"$TMP_DIR/gate-22cm-flip.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] && grep -q 'GATE_SCRIPT_NOT_EXECUTABLE' "$TMP_DIR/gate-22cm-flip.log"; then
        t_pass "identity gate: a COMMITTED mode flip is still refused on a mode-blind checkout"
    else
        t_fail "identity gate: the committed mode flip was missed on a mode-blind checkout (rc=$G_RC; see $TMP_DIR/gate-22cm-flip.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        git update-index --chmod=+x scripts/todo-graph/identity-gate.sh >/dev/null 2>&1
        git commit --quiet --no-verify -m "22cm: restore" >/dev/null 2>&1
        if [ -n "$G_CM_FM_WAS" ]; then
            git config core.fileMode "$G_CM_FM_WAS"
        else
            git config --unset core.fileMode 2>/dev/null || true
        fi
        chmod +x scripts/todo-graph/identity-gate.sh
        chmod -x scripts/todo-graph/snapshot_protocol.json
        git checkout --quiet -- scripts/todo-graph >/dev/null 2>&1
    )
    G_CM_FM_NOW="$( (cd "$GATE_REPO" && git config --get core.fileMode 2>/dev/null) )"
    G_CM_LEFT="$( (cd "$GATE_REPO" && git status --porcelain -- scripts/todo-graph) )"
    if [ "$G_CM_FM_NOW" = "$G_CM_FM_WAS" ] && [ -z "$G_CM_LEFT" ]; then
        t_pass "identity gate: CONTROL -- the mode-blind case restores config and worktree exactly"
    else
        t_fail "identity gate: the mode-blind case left core.fileMode [$G_CM_FM_NOW] (was [$G_CM_FM_WAS]) and worktree [$G_CM_LEFT]"
    fi

    # 22cn: A COMMITTED DELETION MUST NOT BE RESURRECTED BY A DIRTY WORKING
    # TREE. While content was read from the worktree, applying that to PRESENCE
    # let a push that DELETES a closure member exit 0: the recreated file hashed
    # to the base OID, the head tree entry was empty so the mode comparison was
    # skipped, and the two sides compared equal (Codex adversarial, section 51
    # round 6, [high]). Section 54 reads both from the commit, so the two can no
    # longer disagree and the deletion measures as a deletion.
    G_CN_BASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    G_CN_OID="$( (cd "$GATE_REPO" && git rev-parse --quiet --verify "HEAD:scripts/todo-graph/producer_differential.py" 2>/dev/null) )"
    (
        cd "$GATE_REPO" || exit 1
        git rm --quiet -f scripts/todo-graph/producer_differential.py >/dev/null 2>&1
        t="$(find todo -name 'TODO-*.md' | sort | head -1)"
        printf '\n- [ ] section 51 fixture: a todo-only edit beside a committed deletion\n' >>"$t"
        git add "$t" >/dev/null 2>&1
        git commit --quiet --no-verify -m "22cn: delete a closure member" >/dev/null 2>&1
        git cat-file -p "$G_CN_OID" > scripts/todo-graph/producer_differential.py 2>/dev/null
    )
    # PREMISE: the resurrected file must hash to exactly the base blob, or the
    # case degenerates into an ordinary content difference.
    G_CN_HOID="$( (cd "$GATE_REPO" && git hash-object scripts/todo-graph/producer_differential.py 2>/dev/null) )"
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CN_BASE" --head HEAD \
        >"$TMP_DIR/gate-22cn.log" 2>&1)
    G_RC=$?
    G_CN_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22cn.log" | head -1)"
    # SECTION 54 CLOSES THIS AT ITS SOURCE RATHER THAN CATCHING IT. The
    # resurrection worked because the head CONTENT reading came from the
    # worktree while the head PRESENCE reading came from the commit; the two
    # disagreed and the presence check was the patch over that. Now both come
    # from the commit, so the deleted member measures as absent, differs from
    # the base, and the range is DIFFERENTIALLED instead of exiting 0 -- which
    # is the honest outcome for a push that removes a resolver file. The premise
    # (the worktree copy really does hash to the base blob) is still asserted:
    # without it the case degenerates into an ordinary content difference and
    # would pass for the wrong reason.
    if [ -n "$G_CN_OID" ] && [ "$G_CN_OID" = "$G_CN_HOID" ] \
       && ! grep -q 'resolver closure byte-identical base\.\.head' "$TMP_DIR/gate-22cn.log" \
       && grep -q 'closure changed: scripts/todo-graph/producer_differential.py' "$TMP_DIR/gate-22cn.log"; then
        t_pass "identity gate: a committed deletion is measured as a deletion, whatever the worktree holds"
    else
        t_fail "identity gate: a committed closure deletion rode the exit (rc=$G_RC; got [$G_CN_TOKEN]; base=$G_CN_OID worktree=$G_CN_HOID; see $TMP_DIR/gate-22cn.log)"
    fi
    # The INVERSE: the head commit carries a member the working tree lacks --
    # the closure analogue of HEAD_PROTOCOL_UNMATERIALIZED. The BASE must lack
    # it too, and that is not a detail: an absent worktree file already differs
    # from a present base in the FIRST pass, so a base that carries it forces an
    # ordinary walk and the case never reaches the code it was written for. The
    # first cut did exactly that and returned rc 0.
    G_CN_MBASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_CN_BASE" -- scripts/todo-graph/producer_differential.py >/dev/null 2>&1
        t="$(find todo -name 'TODO-*.md' | sort | head -1)"
        printf '\n- [ ] section 51 fixture: a todo-only edit beside a committed re-addition\n' >>"$t"
        git add "$t" scripts/todo-graph/producer_differential.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22cn: re-add the closure member" >/dev/null 2>&1
        rm -f scripts/todo-graph/producer_differential.py
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CN_MBASE" --head HEAD \
        >"$TMP_DIR/gate-22cn-missing.log" 2>&1)
    G_RC=$?
    G_CN_MTOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22cn-missing.log" | head -1)"
    # INVERTED BY SECTION 54 (control at 22as). The gate materializes the head
    # commit itself, so "the checkout never materialized this member" is a
    # statement about the operator's tree and not about the range: it is
    # disclosed by name and the commit is adjudicated on its own terms.
    if [ "$G_RC" -ne 3 ] \
       && grep -q 'diverges: scripts/todo-graph/producer_differential.py' "$TMP_DIR/gate-22cn-missing.log" \
       && ! grep -q 'CLOSURE_UNVERIFIABLE' "$TMP_DIR/gate-22cn-missing.log"; then
        t_pass "identity gate: a member the operator's checkout lacks is disclosed, not refused"
    else
        t_fail "identity gate: an unmaterialized closure member still decided the verdict (rc=$G_RC; got [$G_CN_MTOKEN]; see $TMP_DIR/gate-22cn-missing.log)"
    fi
    (cd "$GATE_REPO" && git checkout --quiet -- scripts/todo-graph >/dev/null 2>&1)

    # 22co: A CONTENT CHANGE MUST NOT BUY A FREE PASS ON EVERYTHING ELSE. The
    # structural checks were once hung under `CLOSURE_CHANGED -eq 0`, so the
    # moment any closure blob differed, entry type, committed mode and
    # head-vs-worktree presence all went unexamined -- which meant the exact
    # scenario the mode check exists for still worked: pair a benign content
    # edit with the gate losing its executable bit, and the local hook skips the
    # non-executable script while CI takes the content-changed branch and can
    # return rc 0 from an unchanged differential (Codex adversarial, section 51
    # review, [high]). Fixture 22cl is mode-ONLY and structurally cannot see it.
    G_CO_PRE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        printf '\n# 22co: benign content change riding with a mode flip\n' \
            >>scripts/todo-graph/ref_resolution.py
        git update-index --chmod=-x scripts/todo-graph/identity-gate.sh >/dev/null 2>&1
        git add scripts/todo-graph/ref_resolution.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22co: content change plus a mode flip" >/dev/null 2>&1
    )
    # PREMISE: the content really did change (so the old guard would have
    # skipped the structural checks) AND the mode really did flip.
    G_CO_CONTENT="$( (cd "$GATE_REPO" && git diff --name-only "$G_CO_PRE" HEAD -- scripts/todo-graph/ref_resolution.py) )"
    G_CO_MODE="$( (cd "$GATE_REPO" && git ls-tree HEAD -- scripts/todo-graph/identity-gate.sh 2>/dev/null | cut -d' ' -f1) )"
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CO_PRE" --head HEAD \
        >"$TMP_DIR/gate-22co.log" 2>&1)
    G_RC=$?
    G_CO_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22co.log" | head -1)"
    if [ -n "$G_CO_CONTENT" ] && [ "$G_CO_MODE" = "100644" ] \
       && [ "$G_RC" -eq 3 ] \
       && grep -q 'GATE_SCRIPT_NOT_EXECUTABLE' "$TMP_DIR/gate-22co.log"; then
        t_pass "identity gate: a mode flip riding a content change is still refused"
    else
        t_fail "identity gate: a content change masked a mode flip (rc=$G_RC; got [$G_CO_TOKEN]; content [$G_CO_CONTENT]; mode $G_CO_MODE; see $TMP_DIR/gate-22co.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        git update-index --chmod=+x scripts/todo-graph/identity-gate.sh >/dev/null 2>&1
        git checkout --quiet "$G_CO_PRE" -- scripts/todo-graph/ref_resolution.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22co: restore" >/dev/null 2>&1
        chmod +x scripts/todo-graph/identity-gate.sh
    )

    # 22cp: A NON-REGULAR CLOSURE MEMBER IS REFUSED BEFORE IT CAN BE EXECUTED,
    # and that is the other half of the same guard. 22ck's symlink carries the
    # BASE bytes so it reaches the exit; here the content differs, which under
    # the old guard sent the range straight to the differential -- and the walks
    # then import the closure, so the gate would have EXECUTED bytes reached
    # through a path it declares is not a closure file.
    G_CP_PRE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        rm -f scripts/todo-graph/producer_differential.py
        ln -s ../../scripts/todo-graph/build.py scripts/todo-graph/producer_differential.py
        git add scripts/todo-graph/producer_differential.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22cp: a closure member that is a symlink" >/dev/null 2>&1
    )
    G_CP_MODE="$( (cd "$GATE_REPO" && git ls-tree HEAD -- scripts/todo-graph/producer_differential.py 2>/dev/null | cut -d' ' -f1) )"
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CP_PRE" --head HEAD \
        >"$TMP_DIR/gate-22cp.log" 2>&1)
    G_RC=$?
    G_CP_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22cp.log" | head -1)"
    if [ "$G_CP_MODE" = "120000" ] && [ "$G_RC" -eq 3 ] \
       && grep -q 'CLOSURE_UNVERIFIABLE' "$TMP_DIR/gate-22cp.log" \
       && ! grep -q 'walking with BASE and HEAD resolver code' "$TMP_DIR/gate-22cp.log"; then
        t_pass "identity gate: a symlinked closure member is refused BEFORE the walks execute it"
    else
        t_fail "identity gate: a symlinked closure member reached the differential (rc=$G_RC; got [$G_CP_TOKEN]; mode $G_CP_MODE; see $TMP_DIR/gate-22cp.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        rm -f scripts/todo-graph/producer_differential.py
        git checkout --quiet "$G_CP_PRE" -- scripts/todo-graph/producer_differential.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22cp: restore the closure member" >/dev/null 2>&1
    )

    # 22cq: AND MODE CHANGES ON EVERY OTHER CLOSURE MEMBER ARE NOT REFUSED.
    # This is the control that keeps the invariant narrow. The withdrawn
    # base-vs-head comparison refused all of them, which is what made it a
    # wedge; only identity-gate.sh's own bit is load-bearing, because only it
    # is what `.githooks/pre-push` tests before running the gate.
    G_CQ_PRE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        git update-index --chmod=+x scripts/todo-graph/snapshot_protocol.json >/dev/null 2>&1
        t="$(find todo -name 'TODO-*.md' | sort | head -1)"
        printf '\n- [ ] section 51 fixture: a todo-only edit beside an irrelevant mode change\n' >>"$t"
        git add "$t" >/dev/null 2>&1
        git commit --quiet --no-verify -m "22cq: mode change on a non-gate closure member" >/dev/null 2>&1
    )
    G_CQ_MODE="$( (cd "$GATE_REPO" && git ls-tree HEAD -- scripts/todo-graph/snapshot_protocol.json 2>/dev/null | cut -d' ' -f1) )"
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CQ_PRE" --head HEAD \
        >"$TMP_DIR/gate-22cq.log" 2>&1)
    G_RC=$?
    if [ "$G_CQ_MODE" = "100755" ] && [ "$G_RC" -eq 0 ] \
       && ! grep -qE 'GATE_SCRIPT_NOT_EXECUTABLE|CLOSURE_MODE' "$TMP_DIR/gate-22cq.log"; then
        t_pass "identity gate: a mode change on a non-gate closure member is not refused"
    else
        t_fail "identity gate: an irrelevant mode change was refused (rc=$G_RC; mode $G_CQ_MODE; see $TMP_DIR/gate-22cq.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        git update-index --chmod=-x scripts/todo-graph/snapshot_protocol.json >/dev/null 2>&1
        git commit --quiet --no-verify -m "22cq: restore" >/dev/null 2>&1
    )

    # 22cg: ABSENCE ON BOTH SIDES IS THE ONE NEW REFUSAL THIS MOVE CREATES, and
    # it is the shape that disproved the first draft of the implementation
    # comment: every protocol path hashes MISSING against MISSING, so the
    # closure is byte-identical with no protocol-path difference at all, and the
    # base still earns a token. A tree carrying no snapshot mechanism cannot be
    # gated, so refusing it IS the decision rather than a regression.
    G_CG_PRE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        git rm --quiet -f scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/snapshot_protocol.py \
            scripts/todo-graph/ref_resolution.py \
            scripts/todo-graph/corpus_resolution_snapshot.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22cg: no protocol mechanism at all" >/dev/null 2>&1
    )
    G_CG_BASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        t="$(find todo -name 'TODO-*.md' | sort | head -1)"
        printf '\n- [ ] section 51 fixture: a todo-only edit with no protocol on either side\n' >>"$t"
        git commit --quiet --no-verify -am "22cg: todo-only edit, no protocol either side" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CG_BASE" --head HEAD \
        >"$TMP_DIR/gate-22cg.log" 2>&1)
    G_RC=$?
    G_CG_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22cg.log" | head -1)"
    if [ "$G_RC" -eq 3 ] \
       && grep -qE 'BASE_PROTOCOL_REMOVED|BASE_PREDATES_PROTOCOL' "$TMP_DIR/gate-22cg.log" \
       && ! grep -q 'CLOSURE_UNVERIFIABLE' "$TMP_DIR/gate-22cg.log" \
       && ! grep -q 'resolver closure byte-identical base\.\.head' "$TMP_DIR/gate-22cg.log"; then
        t_pass "identity gate: a byte-identical closure with NO protocol on either side is refused by name"
    else
        t_fail "identity gate: absence on both sides rode the byte-identical exit (rc=$G_RC; got [$G_CG_TOKEN]; see $TMP_DIR/gate-22cg.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_CG_PRE" -- scripts/todo-graph >/dev/null 2>&1
        git commit --quiet --no-verify -m "22cg: restore the protocol mechanism" >/dev/null 2>&1
    )

    # 22ch: THE LEGACY FORM BEHIND THE SAME EXIT. 22cd and 22ce both leave the
    # JSON in place, so both exercise only `proto_of`'s DATA branch -- a
    # form-selective regression that restored the pre-read exit whenever the
    # JSON is absent would pass them both (Codex test-coverage, section 51,
    # [high]). Removing the JSON and its loader leaves the legacy PAIR, which is
    # the form `proto_of` then selects.
    # NO HEALTHY-LEGACY CONTROL IS CONSTRUCTIBLE against this tree, and that is
    # a fact about the tree rather than an omission: since section 18 the legacy
    # names are re-exports of the data loader (`ref_resolution.py:208`), so a
    # legacy pair without the JSON cannot load at all. The reachable legacy
    # assertion is therefore the unreadable one.
    G_CH_PRE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        git rm --quiet -f scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22ch: legacy pair only, and it cannot load" >/dev/null 2>&1
    )
    G_CH_BASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (
        cd "$GATE_REPO" || exit 1
        t="$(find todo -name 'TODO-*.md' | sort | head -1)"
        printf '\n- [ ] section 51 fixture: a todo-only edit over an unreadable legacy pair\n' >>"$t"
        git commit --quiet --no-verify -am "22ch: todo-only edit over the legacy pair" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CH_BASE" --head HEAD \
        >"$TMP_DIR/gate-22ch.log" 2>&1)
    G_RC=$?
    G_CH_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22ch.log" | head -1)"
    if [ "$G_RC" -eq 3 ] \
       && grep -qE 'BASE_PROTOCOL_[A-Z_]+' "$TMP_DIR/gate-22ch.log" \
       && ! grep -q 'resolver closure byte-identical base\.\.head' "$TMP_DIR/gate-22ch.log"; then
        t_pass "identity gate: the legacy form is behind the exit too, not only the data form"
    else
        t_fail "identity gate: an unreadable legacy pair rode the byte-identical exit (rc=$G_RC; got [$G_CH_TOKEN]; see $TMP_DIR/gate-22ch.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_CH_PRE" -- scripts/todo-graph >/dev/null 2>&1
        git commit --quiet --no-verify -m "22ch: restore the data form" >/dev/null 2>&1
    )

    # 22ci: THE TWO EXITS THAT STILL PRECEDE THE PROTOCOL READ DO SO ON PURPOSE.
    # `base == head` and an unusable base are decided before a protocol byte is
    # read, and after section 51 that placement is a DECISION rather than an
    # accident -- but 22a and 22e only ever see a healthy protocol, so moving
    # either one below the read would leave both green (Codex test-coverage,
    # section 51, [medium]). Asked under a MALFORMED protocol, each answer is
    # only reachable from its published position.
    (
        cd "$GATE_REPO" || exit 1
        python3 - scripts/todo-graph/snapshot_protocol.json <<'PY3'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["snapshot_schema"] = "not-an-int"
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY3
        git commit --quiet --no-verify -am "22ci: malformed protocol for the precedence cases" >/dev/null 2>&1
    )
    G_CI_HEAD="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CI_HEAD" --head HEAD \
        >"$TMP_DIR/gate-22ci-selfsame.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 0 ] \
       && grep -q 'base == head; nothing to differentiate' "$TMP_DIR/gate-22ci-selfsame.log" \
       && ! grep -qE 'PROTOCOL_[A-Z_]+' "$TMP_DIR/gate-22ci-selfsame.log"; then
        t_pass "identity gate: base == head is still answered before the protocol is read"
    else
        t_fail "identity gate: base == head fell behind the protocol read (rc=$G_RC; see $TMP_DIR/gate-22ci-selfsame.log)"
    fi
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" \
        --base 0000000000000000000000000000000000000000 --head HEAD \
        >"$TMP_DIR/gate-22ci-badbase.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'is not a commit in this repository' "$TMP_DIR/gate-22ci-badbase.log" \
       && ! grep -qE 'PROTOCOL_[A-Z_]+' "$TMP_DIR/gate-22ci-badbase.log"; then
        t_pass "identity gate: an unusable explicit base still wins over any protocol token"
    else
        t_fail "identity gate: an unusable base was masked by a protocol verdict (rc=$G_RC; see $TMP_DIR/gate-22ci-badbase.log)"
    fi
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet "$G_CH_PRE" -- scripts/todo-graph/snapshot_protocol.json >/dev/null 2>&1
        git commit --quiet --no-verify -am "22ci: restore a well-formed protocol" >/dev/null 2>&1
    )
    if [ -f "$GATE_IN_CLONE" ] \
       && [ -f "$GATE_REPO/scripts/todo-graph/snapshot_protocol.json" ] \
       && [ -f "$GATE_REPO/scripts/todo-graph/ref_resolution.py" ]; then
        t_pass "identity gate: CONTROL -- the section 51 block leaves the fixture clone whole"
    else
        t_fail "identity gate: the section 51 block damaged the shared fixture clone"
    fi

    # 22ar: THE HEAD SIDE IS ITS OWN QUESTION. The two sides shared one loop
    # and one message, so a HEAD problem read as a base problem and the
    # reverse. A tree under test with no snapshot mechanism at all is a broken
    # HEAD, and history is irrelevant to that.
    (
        cd "$GATE_REPO" || exit 1
        git rm -q --ignore-unmatch scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/snapshot_protocol.py \
            scripts/todo-graph/ref_resolution.py \
            scripts/todo-graph/corpus_resolution_snapshot.py >/dev/null 2>&1
        git commit --quiet --no-verify -m "22ar: head with no snapshot mechanism" >/dev/null 2>&1
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22ar.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'HEAD_PROTOCOL_ABSENT' "$TMP_DIR/gate-22ar.log" \
       && ! grep -qE 'BASE_PREDATES_PROTOCOL|BASE_PROTOCOL_REMOVED|BASE_PROTOCOL_UNREADABLE|BASE_PROTOCOL_INCOMPLETE' \
              "$TMP_DIR/gate-22ar.log"; then
        t_pass "identity gate: an absent mechanism at HEAD is reported against HEAD, not the base"
    else
        t_fail "identity gate: absent-at-HEAD was not reported as a HEAD failure (rc=$G_RC; see $TMP_DIR/gate-22ar.log)"
    fi

    # 22as: AN INCOMPLETE CHECKOUT IS NOT AN ABSENT MECHANISM. Asking the
    # filesystem alone cannot tell a sparse or partial materialization from a
    # tree that genuinely lacks the files, and the two need opposite actions:
    # fix the checkout, or fix the tree.
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet HEAD~1 -- scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/snapshot_protocol.py \
            scripts/todo-graph/ref_resolution.py \
            scripts/todo-graph/corpus_resolution_snapshot.py >/dev/null 2>&1
        git commit --quiet --no-verify -am "22as: restore the mechanism in the commit" >/dev/null 2>&1
        rm -f scripts/todo-graph/snapshot_protocol.json \
              scripts/todo-graph/snapshot_protocol.py \
              scripts/todo-graph/ref_resolution.py \
              scripts/todo-graph/corpus_resolution_snapshot.py
    )
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
        >"$TMP_DIR/gate-22as.log" 2>&1)
    G_RC=$?
    G_AS_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22as.log" | head -1)"
    # INVERTED BY SECTION 54, and this is the whole point of that section. The
    # gate materializes the head commit itself, so a checkout the OPERATOR broke
    # is no longer the tree under test: it cannot make a sound commit refuse,
    # and -- the half that actually mattered -- it cannot make a broken commit
    # pass. What it MUST do is say the tree it read was not the one on disk.
    if [ "$G_RC" -ne 3 ] \
       && grep -q 'HEAD_WORKTREE_DIVERGES' "$TMP_DIR/gate-22as.log" \
       && grep -q 'diverges: scripts/todo-graph/snapshot_protocol.json' "$TMP_DIR/gate-22as.log" \
       && ! grep -q 'HEAD_PROTOCOL_UNMATERIALIZED' "$TMP_DIR/gate-22as.log"; then
        t_pass "identity gate: a broken checkout of a sound commit is disclosed, not adjudicated"
    else
        t_fail "identity gate: an operator's broken checkout still decided the verdict (rc=$G_RC; got [$G_AS_TOKEN]; see $TMP_DIR/gate-22as.log)"
    fi
    # THE CONTROL FOR EVERY INVERTED CASE BELOW. Put the head reads back on the
    # working tree and this exact tree refuses again -- so "the gate ignores it
    # now" is measured against "the gate acted on it before", not against
    # nothing. Built once, on the broadest condition of the family.
    G_WTGATE="$GATE_REPO/scripts/todo-graph/identity-gate-wtread.sh"
    if g_s54_wtgate "$G_WTGATE"; then
        (cd "$GATE_REPO" && bash "$G_WTGATE" --base "$G_PROTO_BASE" --head HEAD \
            >"$TMP_DIR/gate-22as-mut.log" 2>&1)
        G_AS_MRC=$?
        if [ "$G_AS_MRC" -eq 3 ] \
           && grep -q 'HEAD_PROTOCOL_UNMATERIALIZED' "$TMP_DIR/gate-22as-mut.log"; then
            t_pass "identity gate: MUTATION -- reading the worktree makes the broken checkout decide again, so 22as measures the source"
        else
            t_fail "identity gate: the working-tree-read mutation did not refuse, so the inverted cases prove nothing (rc=$G_AS_MRC; see $TMP_DIR/gate-22as-mut.log)"
        fi
    else
        t_fail "identity gate: could not build the working-tree-read control (the head-read shape moved)"
    fi
    rm -f "$G_WTGATE"
    (
        cd "$GATE_REPO" || exit 1
        git checkout --quiet -- scripts/todo-graph/snapshot_protocol.json \
            scripts/todo-graph/snapshot_protocol.py \
            scripts/todo-graph/ref_resolution.py \
            scripts/todo-graph/corpus_resolution_snapshot.py >/dev/null 2>&1
    )



    # A snapshot lacking the pre/post halves cannot adjudicate a relocation,
    # and absence is not equality: it must be rc 3, not a clean pass.
    SNAP_HALVES="$TMP_DIR/snap-halves.json"
    SNAP_NOHALVES="$TMP_DIR/snap-nohalves.json"
    STUB_LINT_CACHE="$GATE_LIVE_CACHE" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
        python3 "$SNAP" write "$SNAP_HALVES" >/dev/null 2>&1 \
        || t_fail "corpus snapshot: could not write a halves-bearing snapshot for the fixture"
    python3 - "$SNAP_HALVES" "$SNAP_NOHALVES" <<'PY2' 2>/dev/null || true
import json, sys, pathlib
d = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
d.pop("buckets_pre", None); d.pop("buckets_post", None)
pathlib.Path(sys.argv[2]).write_text(json.dumps(d), encoding="utf-8")
PY2
    if [ ! -s "$SNAP_NOHALVES" ]; then
        t_fail "corpus snapshot: the halves-less fixture could not be built (a silent skip would read as a pass)"
    else
        SNAP_NH_RC=0
        python3 "$SNAP" compare "$SNAP_NOHALVES" "$SNAP_HALVES" --strict \
            >/dev/null 2>"$TMP_DIR/snap-nohalves.err" || SNAP_NH_RC=$?
        if [ "$SNAP_NH_RC" -eq 3 ] && grep -q 'cannot be adjudicated' "$TMP_DIR/snap-nohalves.err"; then
            t_pass "corpus snapshot: a snapshot without bucket halves is rc 3, not a silent pass"
        else
            t_fail "corpus snapshot: halves-less snapshot passed (rc=$SNAP_NH_RC)"
        fi
        SNAP_NH2_RC=0
        SNAP_GOOD_B64="$(python3 -c 'import base64, json, sys; d = json.load(open(sys.argv[1])); print(base64.b64encode(json.dumps([d["buckets_pre"], d["buckets_post"]]).encode()).decode())' "$SNAP_HALVES")"
        python3 "$SNAP" compare "$SNAP_NOHALVES" "$SNAP_HALVES" --strict \
            --base-halves-b64 "$SNAP_GOOD_B64" >/dev/null 2>&1 || SNAP_NH2_RC=$?
        if [ "$SNAP_NH2_RC" -eq 0 ]; then
            t_pass "corpus snapshot: supplied half VALUES restore the comparison"
        else
            t_fail "corpus snapshot: supplied halves did not restore the comparison (rc=$SNAP_NH2_RC)"
        fi
        # AND THE VALUES ARE COMPARED, NOT TRUSTED: halves describing a
        # relocation must FAIL, which a bare attestation flag could never do.
        # The move is pre's LAST element to post's FRONT specifically, because
        # that is the only shape that leaves the flattened vocabulary identical
        # -- which is both what the binding requires and exactly the relocation
        # class this mechanism exists to catch.
        SNAP_BAD_B64="$(python3 -c 'import base64, json, sys; d = json.load(open(sys.argv[1])); pre = list(d["buckets_pre"]); post = list(d["buckets_post"]); post.insert(0, pre.pop()); print(base64.b64encode(json.dumps([pre, post]).encode()).decode())' "$SNAP_HALVES")"
        SNAP_NH3_RC=0
        python3 "$SNAP" compare "$SNAP_NOHALVES" "$SNAP_HALVES" --strict \
            --base-halves-b64 "$SNAP_BAD_B64" >"$TMP_DIR/snap-reloc.out" 2>&1 || SNAP_NH3_RC=$?
        if [ "$SNAP_NH3_RC" -eq 1 ] && grep -q 'RELOCATED' "$TMP_DIR/snap-reloc.out"; then
            t_pass "corpus snapshot: supplied halves are COMPARED, not trusted -- a relocation still fails"
        else
            t_fail "corpus snapshot: supplied halves were trusted rather than compared (rc=$SNAP_NH3_RC)"
        fi
        # AND THE VALUES ARE BOUND TO THE SNAPSHOT'S OWN VOCABULARY: a caller
        # may say how that vocabulary is SPLIT, never invent what it contains.
        SNAP_FAKE_B64="$(python3 -c 'import base64, json; print(base64.b64encode(json.dumps([["totally", "invented"], ["halves", "here"]]).encode()).decode())')"
        SNAP_NH4_RC=0
        python3 "$SNAP" compare "$SNAP_NOHALVES" "$SNAP_HALVES" --strict \
            --base-halves-b64 "$SNAP_FAKE_B64" >"$TMP_DIR/snap-fake.out" 2>&1 || SNAP_NH4_RC=$?
        if [ "$SNAP_NH4_RC" -eq 3 ] && grep -q 'never what it contains' "$TMP_DIR/snap-fake.out"; then
            t_pass "corpus snapshot: fabricated supplied halves are refused, not accepted as evidence"
        else
            t_fail "corpus snapshot: fabricated halves were accepted (rc=$SNAP_NH4_RC)"
        fi
    fi

    (cd "$GATE_REPO" && git checkout --quiet "$G_PROTO_BASE" -- scripts/todo-graph/ref_resolution.py \
        scripts/todo-graph/snapshot_protocol.json scripts/todo-graph/build.py \
        && git commit --quiet --no-verify -am "restore after protocol/producer fixtures" >/dev/null 2>&1)

# ----------------------------------------------------------------------
# CONSUMER DELEGATION (section 18). The identity gate walks the snapshot and
# adjudicates NOTHING about scripts/lint/check_stub_behind_stamp.py, even
# though section 14 unified both behind one `resolve_ref`. This is the
# invariant standing in for a full consumer differential.
#
# MUTATION-CHECKED, because the failure this roadmap keeps paying for is a
# check that is green for the wrong reason -- and section 16's first fixture
# round was exactly that. The mutation is deliberately the SUBTLE bypass: a
# module-level captured reference (so patching the module attribute no longer
# reaches the call) plus a dead `_rr.resolve_ref` call that satisfies a
# count-only structural rule. A check that only counted call sites would pass
# it.
# ----------------------------------------------------------------------
DELEG_CHK="$REPO_ROOT/scripts/lint/check_consumer_delegation.py"
if [ ! -f "$DELEG_CHK" ]; then
    t_fail "consumer delegation: check_consumer_delegation.py is missing"
else
    DELEG_ROOT="$TMP_DIR/deleg-root"
    mkdir -p "$DELEG_ROOT/scripts/lint" "$DELEG_ROOT/scripts/todo-graph"
    cp "$REPO_ROOT"/scripts/todo-graph/*.py "$DELEG_ROOT/scripts/todo-graph/" 2>/dev/null
    cp "$REPO_ROOT"/scripts/todo-graph/*.json "$DELEG_ROOT/scripts/todo-graph/" 2>/dev/null
    cp "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" "$DELEG_ROOT/scripts/lint/"
    cp "$DELEG_CHK" "$DELEG_ROOT/scripts/lint/"

    DELEG_RC=0
    STUB_LINT_CACHE="$GATE_LIVE_CACHE" \
        python3 "$DELEG_ROOT/scripts/lint/check_consumer_delegation.py" \
        >"$TMP_DIR/deleg-clean.log" 2>&1 || DELEG_RC=$?
    if [ "$DELEG_RC" -eq 0 ]; then
        t_pass "consumer delegation: the shipped consumer delegates every occurrence"
    else
        t_fail "consumer delegation: shipped consumer flagged (rc=$DELEG_RC; see $TMP_DIR/deleg-clean.log)"
    fi

    python3 - "$DELEG_ROOT/scripts/lint/check_stub_behind_stamp.py" <<'PY3'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); s = p.read_text(encoding="utf-8")
old = "                result = _rr.resolve_ref(ref, scope, repo_resolved)"
assert old in s, "consumer resolve_ref call site not found"
s = s.replace(old, "                result = _local_resolve(ref, scope, repo_resolved)", 1)
inject = [
    "_CAPTURED = _rr.resolve_ref",
    "",
    "",
    "def _local_resolve(ref, scope, root):",
    "    if False:",
    "        _rr.resolve_ref(ref, scope, root)   # dead call: defeats a count-only rule",
    "    return _CAPTURED(ref, scope, root)      # identical shape, bypasses the patch",
    "",
    "",
    "def _walk(nodes: list, repo_root: Path) -> tuple:",
]
s = s.replace("def _walk(nodes: list, repo_root: Path) -> tuple:", "\n".join(inject), 1)
p.write_text(s, encoding="utf-8")
PY3
    DELEG_MUT_RC=0
    STUB_LINT_CACHE="$GATE_LIVE_CACHE" \
        python3 "$DELEG_ROOT/scripts/lint/check_consumer_delegation.py" \
        >"$TMP_DIR/deleg-mutated.log" 2>&1 || DELEG_MUT_RC=$?
    if [ "$DELEG_MUT_RC" -eq 1 ] \
       && grep -q 'called the shared resolve_ref 0 time' "$TMP_DIR/deleg-mutated.log"; then
        t_pass "consumer delegation: mutation-check -- a captured-reference bypass FAILS the check"
    else
        t_fail "consumer delegation: bypass not caught (rc=$DELEG_MUT_RC; see $TMP_DIR/deleg-mutated.log)"
    fi

    # The SUBTLER consumer bypass: honour the one class a single-sentinel check
    # would exercise and re-derive every other. A check that forced one bucket
    # passes this; the per-class rotation is what catches it (Codex
    # adversarial, section 18).
    cp "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" "$DELEG_ROOT/scripts/lint/"
    python3 - "$DELEG_ROOT/scripts/lint/check_stub_behind_stamp.py" <<'PY3'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); s = p.read_text(encoding="utf-8")
old = "                result = _rr.resolve_ref(ref, scope, repo_resolved)"
assert old in s, "consumer resolve_ref call site not found"
inject = [
    old,
    '                if result.bucket != "path_escape":',
    "                    result = _CAPTURED2(ref, scope, repo_resolved)",
]
s = s.replace(old, "\n".join(inject), 1)
s = s.replace("def _walk(nodes: list, repo_root: Path) -> tuple:",
              "_CAPTURED2 = _rr.resolve_ref\n\n\ndef _walk(nodes: list, repo_root: Path) -> tuple:", 1)
p.write_text(s, encoding="utf-8")
PY3
    DELEG_CLS_RC=0
    STUB_LINT_CACHE="$GATE_LIVE_CACHE" \
        python3 "$DELEG_ROOT/scripts/lint/check_consumer_delegation.py" \
        >"$TMP_DIR/deleg-class.log" 2>&1 || DELEG_CLS_RC=$?
    if [ "$DELEG_CLS_RC" -eq 1 ] \
       && grep -q 'per-class shortfall' "$TMP_DIR/deleg-class.log"; then
        t_pass "consumer delegation: mutation-check -- a BUCKET-KEYED fallback FAILS the check"
    else
        t_fail "consumer delegation: bucket-keyed fallback not caught (rc=$DELEG_CLS_RC; see $TMP_DIR/deleg-class.log)"
    fi

    # The SUBTLEST consumer divergence: a SYMMETRIC SWAP of two classes. Under
    # a rotating single-pass check every class receives an equal share, so the
    # cardinalities still matched and the swap passed with zero violations
    # (Codex adversarial, section 18 round 2). Isolated per-class passes are
    # what make it visible.
    cp "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" "$DELEG_ROOT/scripts/lint/"
    python3 - "$DELEG_ROOT/scripts/lint/check_stub_behind_stamp.py" <<'PY3'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); s = p.read_text(encoding="utf-8")
old = "                result = _rr.resolve_ref(ref, scope, repo_resolved)"
assert old in s, "consumer resolve_ref call site not found"
# SWAPPED WITH LEGAL ENUM MEMBERS (section 20). Staging the swap with raw
# strings now trips the resolver's bucket contract inside `_replace`, so the
# probe died as rc 3 before the delegation check could report anything -- the
# fixture would have been asserting the wrong mechanism. Using members the
# resolver really emits makes this a STRONGER test: a consumer post-processing
# verdicts with entirely valid values must still be caught as a VIOLATION.
inject = [
    old,
    '                if result.bucket == _rr.Bucket.MISSING_FILE:',
    '                    result = result._replace(bucket=_rr.Bucket.UNRESOLVED_CALLLIKE)',
    '                elif result.bucket == _rr.Bucket.UNRESOLVED_CALLLIKE:',
    '                    result = result._replace(bucket=_rr.Bucket.MISSING_FILE)',
]
p.write_text(s.replace(old, "\n".join(inject), 1), encoding="utf-8")
PY3
    DELEG_SWAP_RC=0
    STUB_LINT_CACHE="$GATE_LIVE_CACHE" \
        python3 "$DELEG_ROOT/scripts/lint/check_consumer_delegation.py" \
        >"$TMP_DIR/deleg-swap.log" 2>&1 || DELEG_SWAP_RC=$?
    if [ "$DELEG_SWAP_RC" -eq 1 ] \
       && grep -q 'reached by the consumer itself' "$TMP_DIR/deleg-swap.log"; then
        t_pass "consumer delegation: mutation-check -- an equal-cardinality class SWAP FAILS the check"
    else
        t_fail "consumer delegation: class swap not caught (rc=$DELEG_SWAP_RC; see $TMP_DIR/deleg-swap.log)"
    fi

    # A RESOLVED verdict's COORDINATES must propagate, not just its count: a
    # consumer that inspects a different body reports on code the shared rule
    # never pointed at.
    cp "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" "$DELEG_ROOT/scripts/lint/"
    python3 - "$DELEG_ROOT/scripts/lint/check_stub_behind_stamp.py" <<'PY3'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); s = p.read_text(encoding="utf-8")
old = ("                stub = _is_stub_cached(result.def_abs, result.line_start,\n"
       "                                       result.line_end)")
assert old in s, "stub inspection call not found"
p.write_text(s.replace(old, "                stub = _is_stub_cached(result.def_abs, 1, 2)", 1),
             encoding="utf-8")
PY3
    DELEG_COORD_RC=0
    STUB_LINT_CACHE="$GATE_LIVE_CACHE" \
        python3 "$DELEG_ROOT/scripts/lint/check_consumer_delegation.py" \
        >"$TMP_DIR/deleg-coord.log" 2>&1 || DELEG_COORD_RC=$?
    if [ "$DELEG_COORD_RC" -eq 1 ] \
       && grep -q 'propagate verbatim into the body inspection' "$TMP_DIR/deleg-coord.log"; then
        t_pass "consumer delegation: mutation-check -- rewritten resolved COORDINATES FAIL the check"
    else
        t_fail "consumer delegation: coordinate rewrite not caught (rc=$DELEG_COORD_RC; see $TMP_DIR/deleg-coord.log)"
    fi

    # A consumer that reports the AUTHORED path instead of the location the
    # shared rule returned. The seven passes only reached the "not a stub"
    # branch before, so the publishing path -- the consumer's real output --
    # was never exercised at all.
    cp "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" "$DELEG_ROOT/scripts/lint/"
    python3 - "$DELEG_ROOT/scripts/lint/check_stub_behind_stamp.py" <<'PY3'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); s = p.read_text(encoding="utf-8")
old = "                report_rel = result.def_rel"
assert old in s, "report_rel anchor not found"
p.write_text(s.replace(old, '                report_rel = file_rel or "?"', 1), encoding="utf-8")
PY3
    DELEG_REP_RC=0
    STUB_LINT_CACHE="$GATE_LIVE_CACHE" \
        python3 "$DELEG_ROOT/scripts/lint/check_consumer_delegation.py" \
        >"$TMP_DIR/deleg-report.log" 2>&1 || DELEG_REP_RC=$?
    if [ "$DELEG_REP_RC" -eq 1 ] \
       && grep -q 'must come from the shared rule' "$TMP_DIR/deleg-report.log"; then
        t_pass "consumer delegation: mutation-check -- a REPORTED-PATH rewrite FAILS the check"
    else
        t_fail "consumer delegation: reported-path rewrite not caught (rc=$DELEG_REP_RC; see $TMP_DIR/deleg-report.log)"
    fi

    # A post-resolution bucket must report the path actually OPENED, and that
    # path must come from the shared rule. Checking only list length left this
    # second output path unverified.
    cp "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" "$DELEG_ROOT/scripts/lint/"
    python3 - "$DELEG_ROOT/scripts/lint/check_stub_behind_stamp.py" <<'PY3'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); s = p.read_text(encoding="utf-8")
old = '                        cov[result.bucket].append(f"{result.rel_path}:{symbol}")'
assert old in s, "post-bucket coverage anchor not found"
new = '                        cov[result.bucket].append(f"{file_rel}:{symbol}")'
p.write_text(s.replace(old, new, 1), encoding="utf-8")
PY3
    DELEG_PATH_RC=0
    STUB_LINT_CACHE="$GATE_LIVE_CACHE" \
        python3 "$DELEG_ROOT/scripts/lint/check_consumer_delegation.py" \
        >"$TMP_DIR/deleg-relpath.log" 2>&1 || DELEG_PATH_RC=$?
    if [ "$DELEG_PATH_RC" -eq 1 ] \
       && grep -q 'do not carry the rel_path' "$TMP_DIR/deleg-relpath.log"; then
        t_pass "consumer delegation: mutation-check -- an AUTHORED-path post-bucket sample FAILS the check"
    else
        t_fail "consumer delegation: post-bucket path rewrite not caught (rc=$DELEG_PATH_RC; see $TMP_DIR/deleg-relpath.log)"
    fi

    # A data-only bucket ADDITION is a supported migration (fixture 22j), so
    # the lint consumer must survive one. While it kept a PRIVATE copy of the
    # vocabulary, `cov[result.bucket]` raised KeyError on the new name and
    # Check 25 exited 3 -- the migration the gate advertises could not pass
    # lint (Codex consistency, section 18 review).
    cp "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" "$DELEG_ROOT/scripts/lint/"
    python3 - "$DELEG_ROOT/scripts/todo-graph/snapshot_protocol.json" <<'PY3'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["post_resolution_buckets"].append("an_added_but_unused_bucket")
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY3
    DELEG_ADD_RC=0
    STUB_LINT_CACHE="$GATE_LIVE_CACHE" \
        python3 "$DELEG_ROOT/scripts/lint/check_consumer_delegation.py" \
        >"$TMP_DIR/deleg-added.log" 2>&1 || DELEG_ADD_RC=$?
    if [ "$DELEG_ADD_RC" -eq 0 ]; then
        t_pass "consumer delegation: a data-only bucket ADDITION does not break the consumer"
    else
        t_fail "consumer delegation: an added protocol bucket broke the consumer (rc=$DELEG_ADD_RC; see $TMP_DIR/deleg-added.log)"
    fi

fi


    # 22i: A MALFORMED CACHE MUST BE rc 3, NOT rc 1. `collect()` uses
    # `section_n` as a dict key, so a list-valued one is unhashable and raised
    # an uncaught TypeError -- python exits 1, which is this tool's DOCUMENTED
    # code for "a prior verdict changed", so the gate would report a malformed
    # cache to CI as a resolver REGRESSION (Codex consistency, section 16).
    python3 - "$TMP_DIR/bad-cache.json" <<'PY'
import json, sys
# Every OTHER field is schema-valid on purpose, so the unhashable `section_n`
# is what the validator actually rejects. Before section 17 this item also
# omitted `item_text`; once the shared validator began enforcing the documented
# required-field list, that omission would have fired FIRST and this fixture
# would have silently stopped testing the property it names.
json.dump([{"file_path": "todo/x.md",
            "stamped_items": [{"section_n": ["not", "hashable"], "item_idx": 0,
                               "item_text": "x",
                               "refs": [{"kind": "symbol", "file": "a.c",
                                         "symbol": "f"}]}]}],
          open(sys.argv[1], "w"))
PY
    (cd "$GATE_REPO" && STUB_LINT_CACHE="$TMP_DIR/bad-cache.json" STUB_LINT_REPO_ROOT="$GATE_REPO" \
        python3 scripts/todo-graph/corpus_resolution_snapshot.py write "$TMP_DIR/nope2.json" \
        >"$TMP_DIR/gate-22i.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ]; then
        t_pass "identity gate: a malformed cache is rc 3, not rc 1 (regression)"
    else
        t_fail "identity gate: malformed cache exited $G_RC, expected 3 (see $TMP_DIR/gate-22i.log)"
    fi

    # 22f: --strict is what makes an unreviewed GAINED fail for a machine.
    # Assert the flag is honoured rather than silently ignored: `write
    # --strict` is a usage error (2), because a no-op flag a caller believes
    # is protecting them is worse than an absent one.
    (cd "$GATE_REPO" && python3 scripts/todo-graph/corpus_resolution_snapshot.py \
        write "$TMP_DIR/nope.json" --strict >"$TMP_DIR/gate-22f.log" 2>&1)
    GATE_RC=$?
    if [ "$GATE_RC" -eq 2 ]; then
        t_pass "identity gate: --strict on write is a usage error, not a no-op"
    else
        t_fail "identity gate: write --strict exited $GATE_RC, expected 2 (see $TMP_DIR/gate-22f.log)"
    fi

    # ------------------------------------------------------------------
    # Section 52: the replacement routes section 50 did not reach.
    # ------------------------------------------------------------------
    # 22cr: A READABLE REPLACEMENT OF THE BASE. 22bh replaces the base with a
    # MALFORMED tree, so it is caught by a guard that ran only once `proto_of`
    # had already failed. The consequential member of the family is the
    # opposite one: substitute the base with a tree that PARSES PERFECTLY and
    # nothing ever asks the question, so the gate reaches a real verdict --
    # here a byte-identical closure against the head's own tree -- about a base
    # the repository manufactured. This case changes a PASS, not a refusal.
    G_S52_HEAD="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    if [ -z "$G_S52_HEAD" ] || [ -z "$G_PROTO_BASE" ]; then
        t_fail "identity gate: no endpoints available for the section 52 cases"
    else
        (cd "$GATE_REPO" && git replace -f "$G_PROTO_BASE" "$G_S52_HEAD" >/dev/null 2>&1)
        if [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
            t_fail "identity gate: the readable base replacement was not created (the case would pass vacuously)"
        else
            (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
                >"$TMP_DIR/gate-22cr.log" 2>&1)
            G_RC=$?
            if [ "$G_RC" -eq 3 ] \
               && grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22cr.log" \
               && ! grep -q 'BASE_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22cr.log"; then
                t_pass "identity gate: a base replaced by a PARSEABLE tree is still repository metadata"
            else
                t_fail "identity gate: a readable manufactured base earned a verdict (rc=$G_RC; see $TMP_DIR/gate-22cr.log)"
            fi
            # THE REFUSAL COMES FROM THE SUBSTITUTION BEING ACTIVE, not from
            # the ref merely existing -- and this is also the documented
            # opt-out: an operator who asks git for stored objects gets the
            # adjudication a fresh CI clone would get, because replace refs are
            # not fetched by default.
            (cd "$GATE_REPO" && GIT_NO_REPLACE_OBJECTS=1 bash "$GATE_IN_CLONE" \
                --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22cr-optout.log" 2>&1)
            if ! grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22cr-optout.log"; then
                t_pass "identity gate: CONTROL -- GIT_NO_REPLACE_OBJECTS opts out of the substitution refusal"
            else
                t_fail "identity gate: the refusal fired while git was reading stored objects (see $TMP_DIR/gate-22cr-optout.log)"
            fi
            (cd "$GATE_REPO" && git replace -d "$G_PROTO_BASE" >/dev/null 2>&1)
            # MUTATION CONTROL: with the replacement gone the same invocation
            # must stop refusing, or 22cr proves only that something broke.
            (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
                >"$TMP_DIR/gate-22cr-control.log" 2>&1)
            if ! grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22cr-control.log"; then
                t_pass "identity gate: CONTROL -- an unreplaced base is not called manufactured"
            else
                t_fail "identity gate: the base refusal survived the replacement's removal (see $TMP_DIR/gate-22cr-control.log)"
            fi
        fi

        # 22cs: THE HEAD COMMIT GETS THE SAME QUESTION, which it never got at
        # all. The head side reads that commit for closure-member presence and
        # mode, for the executable-bit invariant, and for the
        # commit-versus-worktree protocol comparison; substituting it makes
        # every one of those answers repository metadata.
        (cd "$GATE_REPO" && git replace -f "$G_S52_HEAD" "$G_PROTO_BASE" >/dev/null 2>&1)
        if [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
            t_fail "identity gate: the head replacement was not created (the case would pass vacuously)"
        else
            (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
                >"$TMP_DIR/gate-22cs.log" 2>&1)
            G_RC=$?
            if [ "$G_RC" -eq 3 ] && grep -q 'HEAD_OBJECTS_SUBSTITUTED' "$TMP_DIR/gate-22cs.log"; then
                t_pass "identity gate: a substituted head commit is refused, not adjudicated"
            else
                t_fail "identity gate: the head endpoint was read through a replacement (rc=$G_RC; see $TMP_DIR/gate-22cs.log)"
            fi
            (cd "$GATE_REPO" && git replace -d "$G_S52_HEAD" >/dev/null 2>&1)
            (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
                >"$TMP_DIR/gate-22cs-control.log" 2>&1)
            if ! grep -q 'HEAD_OBJECTS_SUBSTITUTED' "$TMP_DIR/gate-22cs-control.log"; then
                t_pass "identity gate: CONTROL -- an unreplaced head is not called substituted"
            else
                t_fail "identity gate: the head refusal survived the replacement's removal (see $TMP_DIR/gate-22cs-control.log)"
            fi
        fi

        # 22ct: THE HEAD QUESTION IS SCOPED TO WHAT THE HEAD SIDE ACTUALLY
        # READS. Reusing the base's full-ancestry walk would refuse on a
        # replacement targeting an ancestor of the head -- an object no
        # head-side read can reach, since every one of them is an `ls-tree` of
        # the head commit's own tree (Codex design review, section 52,
        # [medium]). Deterministic false refusal, so it gets its own control.
        G_S52_ANC="$( (cd "$GATE_REPO" && git rev-parse --verify --quiet 'HEAD~1^{commit}' 2>/dev/null) )"
        if [ -z "$G_S52_ANC" ]; then
            t_fail "identity gate: no head ancestor available for the scope control"
        else
            (cd "$GATE_REPO" && git replace -f "$G_S52_ANC" "$G_PROTO_BASE" >/dev/null 2>&1)
            if [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
                t_fail "identity gate: the ancestor replacement was not created (the case would pass vacuously)"
            else
                (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
                    >"$TMP_DIR/gate-22ct.log" 2>&1)
                if ! grep -qE 'HEAD_OBJECTS_SUBSTITUTED|BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22ct.log"; then
                    t_pass "identity gate: a replacement the head side cannot read leaves the verdict alone"
                else
                    t_fail "identity gate: an unreadable-by-head replacement condemned the range (see $TMP_DIR/gate-22ct.log)"
                fi
                (cd "$GATE_REPO" && git replace -d "$G_S52_ANC" >/dev/null 2>&1)
            fi
        fi

        # 22cu: THE ANSWER IS PINNED, NOT MERELY SAMPLED. `git replace -l` is a
        # point-in-time read, so without the export a replacement installed
        # after the guard would substitute every endpoint read that follows it
        # (Codex design review, section 52, [high]). Racing that is not
        # testable; the MECHANISM is. A shim records what
        # GIT_NO_REPLACE_OBJECTS held on each endpoint read, and every one of
        # them must already be pinned.
        G_PIN_LOG="$TMP_DIR/gate-22cu-pin.log"
        rm -f "$G_PIN_LOG"
        G_SHIM_PIN="$TMP_DIR/gitshim-pin-s52"
        mkdir -p "$G_SHIM_PIN"
        {
            printf '#!/usr/bin/env bash\n'
            printf 'PINLOG=%s\n' "$G_PIN_LOG"
            printf 'for a in "$@"; do\n'
            printf '  case "$a" in\n'
            printf '    ls-tree|worktree) printf "%%s\\n" "${GIT_NO_REPLACE_OBJECTS-unset}" >> "$PINLOG"; break ;;\n'
            printf '  esac\n'
            printf 'done\n'
            printf 'exec %s "$@"\n' "$G_REAL_GIT"
        } > "$G_SHIM_PIN/git"
        chmod +x "$G_SHIM_PIN/git"
        (cd "$GATE_REPO" && PATH="$G_SHIM_PIN:$PATH" bash "$GATE_IN_CLONE" \
            --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22cu.log" 2>&1)
        if [ ! -s "$G_PIN_LOG" ]; then
            t_fail "identity gate: the pin probe recorded no endpoint read at all (see $TMP_DIR/gate-22cu.log)"
        elif grep -qx 'unset' "$G_PIN_LOG"; then
            t_fail "identity gate: an endpoint read ran before replacement reads were pinned (see $G_PIN_LOG)"
        else
            t_pass "identity gate: every endpoint read runs with replacement reads pinned off"
        fi

        # 22cv: THE BASE STILL WINS WHEN BOTH ENDPOINTS FAIL. Emitting the head
        # substitution refusal at the point it is COMPUTED put it ahead of every
        # base-side classification, so a malformed base plus a substituted head
        # reported the head and said nothing about the base -- contradicting the
        # base-first precedence the file header publishes, and sending the
        # operator to repair the wrong end of the range (Codex adversarial,
        # section 52 round 2, [medium]). The answer is recorded early and
        # emitted late, which this case is what proves.
        if [ -z "$G_CORRUPT" ]; then
            t_fail "identity gate: no malformed-protocol commit available for the precedence case"
        else
            (cd "$GATE_REPO" && git replace -f "$G_S52_HEAD" "$G_PROTO_BASE" >/dev/null 2>&1)
            if [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
                t_fail "identity gate: the precedence-case head replacement was not created (the case would pass vacuously)"
            else
                (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_CORRUPT" --head HEAD \
                    >"$TMP_DIR/gate-22cv.log" 2>&1)
                G_RC=$?
                if [ "$G_RC" -eq 3 ] \
                   && grep -q 'BASE_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22cv.log" \
                   && ! grep -q 'HEAD_OBJECTS_SUBSTITUTED' "$TMP_DIR/gate-22cv.log"; then
                    t_pass "identity gate: a malformed base still outranks a substituted head"
                else
                    t_fail "identity gate: the head substitution refusal masked a base token (rc=$G_RC; see $TMP_DIR/gate-22cv.log)"
                fi
                (cd "$GATE_REPO" && git replace -d "$G_S52_HEAD" >/dev/null 2>&1)
            fi
        fi

        # 22cw: THE ENDPOINTS ARE RESOLVED UNDER THE PIN, NOT BEFORE IT. The
        # fallback base is `HEAD~1`, and reading the head commit's PARENT
        # resolves through a replacement of that commit -- so with the guard
        # asked after resolution, the gate could derive its base from an object
        # the repository manufactured, and removing the replacement before the
        # guard ran made the metadata look clean (Codex adversarial, section 52
        # round 2, [high]). The race itself is not testable; the RESOLUTION is.
        # A replacement whose substitute has a DIFFERENT parent is installed,
        # and the base the gate reports must be the STORED parent.
        G_S52_STORED_PARENT="$( (cd "$GATE_REPO" && git rev-parse --verify --quiet 'HEAD~1^{commit}' 2>/dev/null) )"
        G_S52_FAKE="$( (cd "$GATE_REPO" && git commit-tree "$(git -C "$GATE_REPO" rev-parse 'HEAD^{tree}')" \
                          -p "$G_PROTO_BASE" -m "22cw: a head whose parent is not the stored one" </dev/null 2>/dev/null) )"
        if [ -z "$G_S52_STORED_PARENT" ] || [ -z "$G_S52_FAKE" ] \
           || [ "$G_S52_STORED_PARENT" = "$G_PROTO_BASE" ]; then
            t_fail "identity gate: could not build the fallback-base resolution fixture"
        else
            (cd "$GATE_REPO" && git replace -f "$G_S52_HEAD" "$G_S52_FAKE" >/dev/null 2>&1)
            if [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
                t_fail "identity gate: the fallback-base replacement was not created (the case would pass vacuously)"
            else
                (cd "$GATE_REPO" && env -u IDENTITY_GATE_LAST_GATED_SHA -u GITHUB_EVENT_BEFORE \
                    bash "$GATE_IN_CLONE" --head HEAD >"$TMP_DIR/gate-22cw.log" 2>&1)
                if grep -q "base $G_S52_STORED_PARENT" "$TMP_DIR/gate-22cw.log" \
                   && ! grep -q "base $G_PROTO_BASE" "$TMP_DIR/gate-22cw.log"; then
                    t_pass "identity gate: the fallback base is resolved from stored objects, not through a replacement"
                else
                    t_fail "identity gate: a manufactured commit decided the fallback base (see $TMP_DIR/gate-22cw.log)"
                fi
                (cd "$GATE_REPO" && git replace -d "$G_S52_HEAD" >/dev/null 2>&1)
            fi
        fi

        # 22cx: AN ENDPOINT REPLACEMENT IS ANSWERED WITHOUT A WALK. A commit is
        # reachable from its own object walk, so enumerating the history to
        # discover that costs the whole closure for an answer already in hand --
        # and on a large repository with an active replacement it can eat most
        # of IDENTITY_GATE_BUDGET_SECS and surface as an indeterminate timeout
        # instead of the refusal (Codex perf, section 52, [medium]). Asserted by
        # OBSERVATION rather than by timing: a shim records whether `rev-list`
        # was spawned at all, and the ancestor case is the probe control proving
        # the shim can see one.
        G_RL_LOG="$TMP_DIR/gate-22cx-revlist.log"
        G_SHIM_RL="$TMP_DIR/gitshim-revlist-s52"
        mkdir -p "$G_SHIM_RL"
        {
            printf '#!/usr/bin/env bash\n'
            printf 'RLLOG=%s\n' "$G_RL_LOG"
            printf 'for a in "$@"; do\n'
            printf '  [ "$a" = "rev-list" ] && { printf "rev-list\\n" >> "$RLLOG"; break; }\n'
            printf 'done\n'
            printf 'exec %s "$@"\n' "$G_REAL_GIT"
        } > "$G_SHIM_RL/git"
        chmod +x "$G_SHIM_RL/git"
        rm -f "$G_RL_LOG"
        (cd "$GATE_REPO" && git replace -f "$G_PROTO_BASE" "$G_S52_HEAD" >/dev/null 2>&1)
        (cd "$GATE_REPO" && PATH="$G_SHIM_RL:$PATH" bash "$GATE_IN_CLONE" \
            --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22cx.log" 2>&1)
        if grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22cx.log" && [ ! -s "$G_RL_LOG" ]; then
            t_pass "identity gate: a replaced endpoint refuses without enumerating its history"
        else
            t_fail "identity gate: an endpoint replacement paid for a full object walk (see $TMP_DIR/gate-22cx.log and $G_RL_LOG)"
        fi
        (cd "$GATE_REPO" && git replace -d "$G_PROTO_BASE" >/dev/null 2>&1)
        # AND THE HEAD ENDPOINT TOO, which the first cut missed. The endpoint
        # test compared the captured list against the commit being asked about,
        # so a sole replacement naming the HEAD was absent from the BASE's test:
        # the base call fell through and walked the whole history to find
        # nothing before the head call short-circuited (Codex perf, section 52
        # round 5, [medium], measured at 70,513 objects). Both endpoints are now
        # removed from the candidate set in one pass, so this variant must spawn
        # no walk either.
        rm -f "$G_RL_LOG"
        (cd "$GATE_REPO" && git replace -f "$G_S52_HEAD" "$G_PROTO_BASE" >/dev/null 2>&1)
        (cd "$GATE_REPO" && PATH="$G_SHIM_RL:$PATH" bash "$GATE_IN_CLONE" \
            --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22cx-head.log" 2>&1)
        if grep -q 'HEAD_OBJECTS_SUBSTITUTED' "$TMP_DIR/gate-22cx-head.log" && [ ! -s "$G_RL_LOG" ]; then
            t_pass "identity gate: a replaced HEAD refuses without walking the base history"
        else
            t_fail "identity gate: a HEAD-only replacement paid for a base walk (see $TMP_DIR/gate-22cx-head.log and $G_RL_LOG)"
        fi
        (cd "$GATE_REPO" && git replace -d "$G_S52_HEAD" >/dev/null 2>&1)
        rm -f "$G_RL_LOG"
        if [ -n "$G_S52_ANC" ]; then
            (cd "$GATE_REPO" && git replace -f "$G_S52_ANC" "$G_PROTO_BASE" >/dev/null 2>&1)
            (cd "$GATE_REPO" && PATH="$G_SHIM_RL:$PATH" bash "$GATE_IN_CLONE" \
                --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22cx-control.log" 2>&1)
            if [ -s "$G_RL_LOG" ]; then
                t_pass "identity gate: CONTROL -- a non-endpoint replacement does still spawn the walk"
            else
                t_fail "identity gate: the rev-list probe never fired, so 22cx passed vacuously"
            fi
            (cd "$GATE_REPO" && git replace -d "$G_S52_ANC" >/dev/null 2>&1)
        fi

        # 22cy: THE WALK STOPS AT THE FIRST MATCH, AND A HIT OUTRANKS THE
        # PRODUCER'S STATUS. 22cx proves only that a walk is or is not spawned;
        # neither leg proves what happens INSIDE one. That mattered because the
        # two arms emit the SAME leading token with different prose -- a
        # definite "an active refs/replace entry substitutes" against an
        # indeterminate "could not determine whether" -- so reading the
        # pipeline status before the HIT would misclassify a known match as
        # unknown and every existing fixture would stay green. Removing awk's
        # `exit` would likewise restore the whole-history scan invisibly (Codex
        # adversarial, section 52 round 4, [medium]).
        #
        # Both are bound here with a CONTROLLED PRODUCER: the shim answers
        # `rev-list` with the replaced oid FIRST and then floods the stream, and
        # writes a drain marker only if it was allowed to finish. An early exit
        # SIGPIPEs it, so the marker's ABSENCE is the termination proof and the
        # definite message is the ordering proof.
        if [ -z "$G_S52_ANC" ]; then
            t_fail "identity gate: no object available for the early-match fixture"
        else
            G_CY_DRAIN="$TMP_DIR/gate-22cy-drain.log"
            G_SHIM_CY="$TMP_DIR/gitshim-earlymatch-s52"
            mkdir -p "$G_SHIM_CY"
            rm -f "$G_CY_DRAIN"
            cat > "$G_SHIM_CY/git" <<SHIM
#!/usr/bin/env bash
DRAINLOG=$G_CY_DRAIN
MATCH=$G_S52_ANC
REALGIT=$G_REAL_GIT
for a in "\$@"; do
  if [ "\$a" = "rev-list" ]; then
    printf '%s tree\n' "\$MATCH"
    # THE PRODUCER'S OWN STATUS IS PROPAGATED, and the SHAPE of that is
    # load-bearing rather than a detail. Two earlier drafts silently exited 0 --
    # first a trailing \`exit 0\`, then \`if ...; then ...; fi; exit \$?\`, which
    # returns 0 because a FAILED \`if\` with no \`else\` evaluates to 0 -- and with
    # either one the mutation that reads the pipeline status before the HIT
    # stayed green, so the probe proved nothing at all. Real git dies of
    # SIGPIPE, so the status is captured directly and returned.
    seq 1 300000 | sed 's/^/0000000000000000000000000000000000000000 /'
    rc=\$?
    [ "\$rc" -eq 0 ] && printf 'drained\n' >> "\$DRAINLOG"
    exit "\$rc"
  fi
done
exec "\$REALGIT" "\$@"
SHIM
            chmod +x "$G_SHIM_CY/git"
            (cd "$GATE_REPO" && git replace -f "$G_S52_ANC" "$G_PROTO_BASE" >/dev/null 2>&1)
            if [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
                t_fail "identity gate: the early-match replacement was not created (the case would pass vacuously)"
            else
                (cd "$GATE_REPO" && PATH="$G_SHIM_CY:$PATH" bash "$GATE_IN_CLONE" \
                    --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22cy.log" 2>&1)
                if grep -q 'an active refs/replace entry substitutes an object reachable from the base' "$TMP_DIR/gate-22cy.log" \
                   && ! grep -q 'could not determine whether repository replacement metadata' "$TMP_DIR/gate-22cy.log"; then
                    t_pass "identity gate: a matched object is reported as substituted, not as indeterminate"
                else
                    t_fail "identity gate: a known substitution was reported as unknown (see $TMP_DIR/gate-22cy.log)"
                fi
                if [ ! -s "$G_CY_DRAIN" ]; then
                    t_pass "identity gate: the object walk is terminated at the first match, not drained"
                else
                    t_fail "identity gate: the walk read the whole stream after a match (see $G_CY_DRAIN)"
                fi
                (cd "$GATE_REPO" && git replace -d "$G_S52_ANC" >/dev/null 2>&1)
            fi
        fi

        # 22cz: ENDPOINT ROLES ARE NOT EXCLUSIVE, AND THE REVERSE RANGE IS THE
        # PROOF. Removing BOTH endpoint oids from one shared candidate set
        # looked symmetric and dropped a reachable replacement: this gate takes
        # arbitrary --base/--head, and after a rewind the last-gated base can be
        # a DESCENDANT of the head, so the head commit is an object the BASE's
        # history walk reaches. With it discarded globally the base answered
        # "not substituted" without walking and the range earned a base
        # classification where BASE_HISTORY_INCOMPLETE was due (Codex
        # adversarial, section 52 round 6, [medium]). Every other case in this
        # block is the forward topology, which cannot see it.
        if [ -z "$G_S52_ANC" ]; then
            t_fail "identity gate: no ancestor available for the reverse-range case"
        else
            (cd "$GATE_REPO" && git replace -f "$G_S52_ANC" "$G_PROTO_BASE" >/dev/null 2>&1)
            if [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
                t_fail "identity gate: the reverse-range replacement was not created (the case would pass vacuously)"
            else
                # base = the NEWER commit, head = its own ancestor, and the
                # replacement names the head.
                (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_S52_HEAD" --head "$G_S52_ANC" \
                    >"$TMP_DIR/gate-22cz.log" 2>&1)
                G_RC=$?
                if [ "$G_RC" -eq 3 ] \
                   && grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22cz.log" \
                   && ! grep -q 'HEAD_OBJECTS_SUBSTITUTED' "$TMP_DIR/gate-22cz.log"; then
                    t_pass "identity gate: a replaced head REACHABLE from the base is the base's refusal"
                else
                    t_fail "identity gate: a reachable replacement was dropped as an endpoint (rc=$G_RC; see $TMP_DIR/gate-22cz.log)"
                fi
                (cd "$GATE_REPO" && git replace -d "$G_S52_ANC" >/dev/null 2>&1)
            fi
        fi

        # 22da: THE PEEL FROM A COMMIT-ISH TO A COMMIT IS ITSELF SUBSTITUTABLE.
        # Endpoints are peeled UNDER the pin, so an annotated tag resolves
        # through the STORED tag object while the operator's own git resolves it
        # through the replacement to a DIFFERENT commit. A tag object is not
        # reachable from the commit it points at, so neither the history walk
        # nor the snapshot walk contains it and every other case in this block
        # is blind to it: the gate would certify a range nobody asked about
        # (Codex adversarial AND consistency, section 52 round 7, [medium],
        # found independently by both). Fixtured on BOTH sides, because the
        # argument can name either one.
        (cd "$GATE_REPO" && git tag -a -f -m "22da: tag a" s52-tag-a "$G_S52_HEAD" >/dev/null 2>&1
         cd "$GATE_REPO" && git tag -a -f -m "22da: tag b" s52-tag-b "$G_PROTO_BASE" >/dev/null 2>&1)
        G_TAG_A="$( (cd "$GATE_REPO" && git rev-parse --verify --quiet s52-tag-a 2>/dev/null) )"
        G_TAG_B="$( (cd "$GATE_REPO" && git rev-parse --verify --quiet s52-tag-b 2>/dev/null) )"
        if [ -z "$G_TAG_A" ] || [ -z "$G_TAG_B" ] || [ "$G_TAG_A" = "$G_S52_HEAD" ]; then
            t_fail "identity gate: could not build the annotated-tag fixture (tags absent, or not tag objects)"
        else
            (cd "$GATE_REPO" && git replace -f "$G_TAG_A" "$G_TAG_B" >/dev/null 2>&1)
            if [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
                t_fail "identity gate: the tag-object replacement was not created (the case would pass vacuously)"
            else
                # PROBE CONTROL: the substitution must actually move the tag for
                # ordinary git, or neither leg below proves anything.
                G_TAG_ACTIVE="$( (cd "$GATE_REPO" && git rev-parse --verify --quiet "s52-tag-a^{commit}" 2>/dev/null) )"
                if [ "$G_TAG_ACTIVE" = "$G_PROTO_BASE" ]; then
                    t_pass "identity gate: CONTROL -- the replaced tag object does move where ordinary git resolves"
                else
                    t_fail "identity gate: the tag replacement did not move git's own resolution (got '$G_TAG_ACTIVE')"
                fi
                (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head s52-tag-a \
                    >"$TMP_DIR/gate-22da-head.log" 2>&1)
                G_RC=$?
                if [ "$G_RC" -eq 3 ] && grep -q 'HEAD_OBJECTS_SUBSTITUTED' "$TMP_DIR/gate-22da-head.log"; then
                    t_pass "identity gate: a head named through a replaced tag object is refused"
                else
                    t_fail "identity gate: the gate adjudicated a head the repository resolves elsewhere (rc=$G_RC; see $TMP_DIR/gate-22da-head.log)"
                fi
                (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base s52-tag-a --head HEAD \
                    >"$TMP_DIR/gate-22da-base.log" 2>&1)
                G_RC=$?
                if [ "$G_RC" -eq 3 ] && grep -q 'BASE_OBJECTS_SUBSTITUTED' "$TMP_DIR/gate-22da-base.log"; then
                    t_pass "identity gate: a base named through a replaced tag object is refused"
                else
                    t_fail "identity gate: the gate adjudicated a base the repository resolves elsewhere (rc=$G_RC; see $TMP_DIR/gate-22da-base.log)"
                fi
                (cd "$GATE_REPO" && git replace -d "$G_TAG_A" >/dev/null 2>&1)
                (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head s52-tag-a \
                    >"$TMP_DIR/gate-22da-control.log" 2>&1)
                if ! grep -qE 'HEAD_OBJECTS_SUBSTITUTED|BASE_OBJECTS_SUBSTITUTED' "$TMP_DIR/gate-22da-control.log"; then
                    t_pass "identity gate: CONTROL -- an unreplaced tag endpoint is not called substituted"
                else
                    t_fail "identity gate: the tag refusal survived the replacement's removal (see $TMP_DIR/gate-22da-control.log)"
                fi
            fi
            # 22db: A REVISION SPEC IS AN ARBITRARY STRING, AND GIT ACCEPTS
            # WHITESPACE IN ONE. `<rev>^{/<regex>}` is a legal endpoint and its
            # regex can contain spaces, so a `spec peel` table looked up by
            # whitespace field made every such spec miss its own row -- and a
            # miss is deliberately read as "no divergence", which reopened the
            # substituted-tag bypass for exactly the specs nobody would think
            # to test (Codex adversarial, section 52 round 9, [medium]).
            G_WS_SPEC='s52-tag-a^{/ }'
            (cd "$GATE_REPO" && git replace -f "$G_TAG_A" "$G_TAG_B" >/dev/null 2>&1)
            G_WS_STORED="$( (cd "$GATE_REPO" && GIT_NO_REPLACE_OBJECTS=1 git rev-parse --verify --quiet "${G_WS_SPEC}^{commit}" 2>/dev/null) )"
            G_WS_ACTIVE="$( (cd "$GATE_REPO" && git rev-parse --verify --quiet "${G_WS_SPEC}^{commit}" 2>/dev/null) )"
            if [ -n "$G_WS_STORED" ] && [ -n "$G_WS_ACTIVE" ] && [ "$G_WS_STORED" != "$G_WS_ACTIVE" ]; then
                t_pass "identity gate: CONTROL -- a whitespace-bearing spec does resolve differently under the replacement"
            else
                t_fail "identity gate: the whitespace spec fixture is vacuous (stored '$G_WS_STORED' active '$G_WS_ACTIVE')"
            fi
            (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head "$G_WS_SPEC" \
                >"$TMP_DIR/gate-22db.log" 2>&1)
            G_RC=$?
            if [ "$G_RC" -eq 3 ] && grep -q 'HEAD_OBJECTS_SUBSTITUTED' "$TMP_DIR/gate-22db.log"; then
                t_pass "identity gate: a whitespace-bearing endpoint spec is not exempt from the peel check"
            else
                t_fail "identity gate: a spec with a space skipped peel-divergence detection (rc=$G_RC; see $TMP_DIR/gate-22db.log)"
            fi
            (cd "$GATE_REPO" && git replace -d "$G_TAG_A" >/dev/null 2>&1)
            (cd "$GATE_REPO" && git tag -d s52-tag-a >/dev/null 2>&1
             cd "$GATE_REPO" && git tag -d s52-tag-b >/dev/null 2>&1)
        fi

        # 22dc: THE EMPTY RANGE IS NOT EXEMPT FROM THE PRECONDITION. Replacing
        # a commit does not move `rev-parse <spec>^{commit}` -- peeling returns
        # the object NAME, and the substitution changes its CONTENT -- so a
        # selfsame invocation cleared the peel check, reached the "nothing to
        # differentiate" exit above the substitution adjudication, and returned
        # a green rc 0 over a commit git shows the operator differently (Codex
        # adversarial, section 52 round 10, [medium]). rc 0 is what a caller
        # records a new baseline on, which is why an empty range still has to
        # answer this.
        (cd "$GATE_REPO" && git replace -f "$G_S52_HEAD" "$G_PROTO_BASE" >/dev/null 2>&1)
        if [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
            t_fail "identity gate: the selfsame-range replacement was not created (the case would pass vacuously)"
        else
            (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_S52_HEAD" --head "$G_S52_HEAD" \
                >"$TMP_DIR/gate-22dc.log" 2>&1)
            G_RC=$?
            if [ "$G_RC" -eq 3 ] \
               && grep -q 'BASE_HISTORY_INCOMPLETE' "$TMP_DIR/gate-22dc.log" \
               && ! grep -q 'nothing to differentiate' "$TMP_DIR/gate-22dc.log"; then
                t_pass "identity gate: a selfsame range over a replaced commit refuses instead of exiting green"
            else
                t_fail "identity gate: an empty range skipped the substitution precondition (rc=$G_RC; see $TMP_DIR/gate-22dc.log)"
            fi
            (cd "$GATE_REPO" && git replace -d "$G_S52_HEAD" >/dev/null 2>&1)
            # MUTATION CONTROL: unreplaced, the same selfsame invocation must
            # still take the empty-range exit, or the fix would have removed it.
            (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_S52_HEAD" --head "$G_S52_HEAD" \
                >"$TMP_DIR/gate-22dc-control.log" 2>&1)
            G_RC=$?
            if [ "$G_RC" -eq 0 ] && grep -q 'nothing to differentiate' "$TMP_DIR/gate-22dc-control.log"; then
                t_pass "identity gate: CONTROL -- an unreplaced selfsame range still exits green"
            else
                t_fail "identity gate: the empty-range exit was lost (rc=$G_RC; see $TMP_DIR/gate-22dc-control.log)"
            fi
        fi

        # 22dd: A MOVING REF IS NOT A SUBSTITUTION. The peel comparison used to
        # measure the ACTIVE peel against the resolution taken after the pin --
        # two reads separated by the whole endpoint resolution -- so an endpoint
        # ref that simply ADVANCED in between diverged for a reason that has
        # nothing to do with replacement metadata, and the gate emitted a token
        # whose published meaning says metadata caused it: a false rc 3 pointing
        # the operator at the wrong repair (Codex consistency, section 52 round
        # 11, [medium]). Both peels are now taken adjacently and the active one
        # TWICE, so movement is detected and named instead of blamed.
        #
        # The race is made deterministic by a shim that moves the branch during
        # the stored read, which is the one call in the pair that carries
        # GIT_NO_REPLACE_OBJECTS in its environment.
        G_MOVE_MARK="$TMP_DIR/gate-22dd-fired"
        rm -f "$G_MOVE_MARK"
        (cd "$GATE_REPO" && git branch -f s52-move-branch "$G_PROTO_BASE" >/dev/null 2>&1)
        (cd "$GATE_REPO" && git replace -f "$G_S52_ANC" "$G_PROTO_BASE" >/dev/null 2>&1)
        G_SHIM_MV="$TMP_DIR/gitshim-move-s52"
        mkdir -p "$G_SHIM_MV"
        cat > "$G_SHIM_MV/git" <<SHIM
#!/usr/bin/env bash
MARK=$G_MOVE_MARK
REALGIT=$G_REAL_GIT
TARGET=$G_CORRUPT
if [ "\${GIT_NO_REPLACE_OBJECTS-}" = "1" ] && [ ! -e "\$MARK" ]; then
  for a in "\$@"; do
    case "\$a" in
      *s52-move-branch*)
        : > "\$MARK"
        "\$REALGIT" branch -f s52-move-branch "\$TARGET" >/dev/null 2>&1
        break ;;
    esac
  done
fi
exec "\$REALGIT" "\$@"
SHIM
        chmod +x "$G_SHIM_MV/git"
        if [ -z "$G_CORRUPT" ] || [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
            t_fail "identity gate: could not build the moving-ref fixture"
        else
            (cd "$GATE_REPO" && PATH="$G_SHIM_MV:$PATH" bash "$GATE_IN_CLONE" \
                --base s52-move-branch --head HEAD >"$TMP_DIR/gate-22dd.log" 2>&1)
            G_RC=$?
            if [ ! -e "$G_MOVE_MARK" ]; then
                t_fail "identity gate: the moving-ref shim never fired, so 22dd would pass vacuously"
            elif [ "$G_RC" -eq 3 ] \
                 && grep -q 'ENDPOINT_MOVED_UNDER_GATE' "$TMP_DIR/gate-22dd.log" \
                 && ! grep -qE 'BASE_OBJECTS_SUBSTITUTED|HEAD_OBJECTS_SUBSTITUTED' "$TMP_DIR/gate-22dd.log"; then
                t_pass "identity gate: an endpoint that moved is named as movement, not as substitution"
            else
                t_fail "identity gate: a moving ref was blamed on replacement metadata (rc=$G_RC; see $TMP_DIR/gate-22dd.log)"
            fi
            (cd "$GATE_REPO" && git replace -d "$G_S52_ANC" >/dev/null 2>&1
             cd "$GATE_REPO" && git branch -D s52-move-branch >/dev/null 2>&1)
        fi

        # 22de: AN UNANSWERED PROBE IS NOT MOVEMENT. The capture takes the
        # active peel twice around the stored one, and only the FIRST two were
        # tested for an answer -- so a failed THIRD peel came back empty, read
        # as "differs from the first", and an indeterminate observation was
        # reported as the endpoint having MOVED. A definite token for a
        # question nobody answered, sending the operator to re-run a repository
        # that was never moving (Codex adversarial AND consistency, section 52
        # round 12, found independently by both).
        G_FAIL_MARK="$TMP_DIR/gate-22de-count"
        rm -f "$G_FAIL_MARK"
        (cd "$GATE_REPO" && git branch -f s52-fail-branch "$G_PROTO_BASE" >/dev/null 2>&1)
        (cd "$GATE_REPO" && git replace -f "$G_S52_ANC" "$G_PROTO_BASE" >/dev/null 2>&1)
        G_SHIM_FAIL="$TMP_DIR/gitshim-thirdpeel-s52"
        mkdir -p "$G_SHIM_FAIL"
        cat > "$G_SHIM_FAIL/git" <<SHIM
#!/usr/bin/env bash
COUNT=$G_FAIL_MARK
REALGIT=$G_REAL_GIT
# The capture reads active, stored, active. Only the two ACTIVE reads run with
# the pin variable absent, so counting those identifies the third peel exactly.
if [ -z "\${GIT_NO_REPLACE_OBJECTS-}" ]; then
  for a in "\$@"; do
    case "\$a" in
      *s52-fail-branch*)
        n=0
        [ -f "\$COUNT" ] && n=\$(cat "\$COUNT")
        n=\$((n + 1))
        printf '%s' "\$n" > "\$COUNT"
        [ "\$n" = "2" ] && exit 128
        break ;;
    esac
  done
fi
exec "\$REALGIT" "\$@"
SHIM
        chmod +x "$G_SHIM_FAIL/git"
        (cd "$GATE_REPO" && PATH="$G_SHIM_FAIL:$PATH" bash "$GATE_IN_CLONE" \
            --base s52-fail-branch --head HEAD >"$TMP_DIR/gate-22de.log" 2>&1)
        G_RC=$?
        if [ ! -f "$G_FAIL_MARK" ] || [ "$(cat "$G_FAIL_MARK" 2>/dev/null)" -lt 2 ]; then
            t_fail "identity gate: the third-peel probe never reached its second active read, so 22de would pass vacuously"
        elif [ "$G_RC" -eq 3 ] \
             && grep -q 'BASE_OBJECTS_SUBSTITUTED' "$TMP_DIR/gate-22de.log" \
             && ! grep -q 'ENDPOINT_MOVED_UNDER_GATE' "$TMP_DIR/gate-22de.log"; then
            t_pass "identity gate: a failed third peel is unanswerable, not movement"
        else
            t_fail "identity gate: an unanswered probe was reported as endpoint movement (rc=$G_RC; see $TMP_DIR/gate-22de.log)"
        fi
        (cd "$GATE_REPO" && git branch -D s52-fail-branch >/dev/null 2>&1)

        # 22df: REPLACEMENT METADATA ADDED AFTER THE CAPTURE IS IN NO SET THE
        # GATE HOLDS. Every substitution answer rests on one `git replace -l`
        # read taken before the endpoints were resolved; an entry landing after
        # it is in neither captured set and, when it targets a tag object, in
        # neither commit walk either -- so the gate could return a GREEN verdict
        # while ordinary git resolves the endpoint through a substitute it never
        # saw. A false pass, not a mis-named refusal (Codex adversarial, section
        # 52 round 12, [medium]).
        G_META_MARK="$TMP_DIR/gate-22df-fired"
        rm -f "$G_META_MARK"
        G_SHIM_META="$TMP_DIR/gitshim-meta-s52"
        mkdir -p "$G_SHIM_META"
        cat > "$G_SHIM_META/git" <<SHIM
#!/usr/bin/env bash
MARK=$G_META_MARK
REALGIT=$G_REAL_GIT
SRC=$G_CORRUPT
DST=$G_PROTO_BASE
seen_replace=0
seen_list=0
for a in "\$@"; do
  [ "\$a" = "replace" ] && seen_replace=1
  [ "\$a" = "-l" ] && seen_list=1
done
if [ "\$seen_replace\$seen_list" = "11" ] && [ ! -e "\$MARK" ]; then
  : > "\$MARK"
  out="\$("\$REALGIT" "\$@")"
  "\$REALGIT" replace -f "\$SRC" "\$DST" >/dev/null 2>&1
  [ -n "\$out" ] && printf '%s\n' "\$out"
  exit 0
fi
exec "\$REALGIT" "\$@"
SHIM
        chmod +x "$G_SHIM_META/git"
        if [ -z "$G_CORRUPT" ]; then
            t_fail "identity gate: no second object available for the metadata-change fixture"
        else
            (cd "$GATE_REPO" && PATH="$G_SHIM_META:$PATH" bash "$GATE_IN_CLONE" \
                --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22df.log" 2>&1)
            G_RC=$?
            if [ ! -e "$G_META_MARK" ]; then
                t_fail "identity gate: the metadata-change shim never fired, so 22df would pass vacuously"
            elif [ "$G_RC" -eq 3 ] && grep -q 'ENDPOINT_MOVED_UNDER_GATE' "$TMP_DIR/gate-22df.log"; then
                t_pass "identity gate: replacement metadata added after the capture is caught, not certified around"
            else
                t_fail "identity gate: metadata landing after the capture went unnoticed (rc=$G_RC; see $TMP_DIR/gate-22df.log)"
            fi
            (cd "$GATE_REPO" && git replace -d "$G_CORRUPT" >/dev/null 2>&1)
        fi
        (cd "$GATE_REPO" && git replace -d "$G_S52_ANC" >/dev/null 2>&1)

        # 22dg: ZERO TO ONE IS THE TRANSITION THAT MATTERED MOST. The listing
        # re-read used to be guarded on the capture having found something, so a
        # repository that starts with NO replacements skipped it entirely and
        # the FIRST replacement created after capture was never seen: the gate
        # pinned reads to stored objects, adjudicated an empty captured set, and
        # returned rc 0 while ordinary git resolved a substituted endpoint
        # (Codex adversarial, section 52 round 13, [medium]). 22df cannot see
        # this because an earlier case leaves a replacement active through it.
        if [ -n "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
            t_fail "identity gate: the zero-to-one fixture needs a repository with no replacements"
        else
            G_ZERO_MARK="$TMP_DIR/gate-22dg-fired"
            rm -f "$G_ZERO_MARK"
            G_SHIM_ZERO="$TMP_DIR/gitshim-zero-s52"
            mkdir -p "$G_SHIM_ZERO"
            cat > "$G_SHIM_ZERO/git" <<SHIM
#!/usr/bin/env bash
MARK=$G_ZERO_MARK
REALGIT=$G_REAL_GIT
SRC=$G_CORRUPT
DST=$G_PROTO_BASE
seen_replace=0
seen_list=0
for a in "\$@"; do
  [ "\$a" = "replace" ] && seen_replace=1
  [ "\$a" = "-l" ] && seen_list=1
done
if [ "\$seen_replace\$seen_list" = "11" ] && [ ! -e "\$MARK" ]; then
  : > "\$MARK"
  "\$REALGIT" replace -f "\$SRC" "\$DST" >/dev/null 2>&1
  exit 0
fi
exec "\$REALGIT" "\$@"
SHIM
            chmod +x "$G_SHIM_ZERO/git"
            (cd "$GATE_REPO" && PATH="$G_SHIM_ZERO:$PATH" bash "$GATE_IN_CLONE" \
                --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22dg.log" 2>&1)
            G_RC=$?
            if [ ! -e "$G_ZERO_MARK" ]; then
                t_fail "identity gate: the zero-to-one shim never fired, so 22dg would pass vacuously"
            elif [ "$G_RC" -eq 3 ] && grep -q 'ENDPOINT_MOVED_UNDER_GATE' "$TMP_DIR/gate-22dg.log"; then
                t_pass "identity gate: the FIRST replacement created after an empty capture is still caught"
            else
                t_fail "identity gate: an empty capture disabled the metadata re-read (rc=$G_RC; see $TMP_DIR/gate-22dg.log)"
            fi
            (cd "$GATE_REPO" && git replace -d "$G_CORRUPT" >/dev/null 2>&1)
        fi

        # 22dh: A FAILED PROBE IS NOT AN OBSERVED CHANGE. The revalidation
        # collapsed a failed listing into a synthetic mismatch, so the gate
        # asserted the metadata CHANGED when it had merely failed to ask -- an
        # unlisted emission reason and a misleading diagnosis under a degraded
        # git (Codex consistency, section 52 round 13, [medium]).
        (cd "$GATE_REPO" && git replace -f "$G_S52_ANC" "$G_PROTO_BASE" >/dev/null 2>&1)
        G_LFAIL_COUNT="$TMP_DIR/gate-22dh-count"
        rm -f "$G_LFAIL_COUNT"
        G_SHIM_LFAIL="$TMP_DIR/gitshim-listfail-s52"
        mkdir -p "$G_SHIM_LFAIL"
        cat > "$G_SHIM_LFAIL/git" <<SHIM
#!/usr/bin/env bash
COUNT=$G_LFAIL_COUNT
REALGIT=$G_REAL_GIT
seen_replace=0
seen_list=0
for a in "\$@"; do
  [ "\$a" = "replace" ] && seen_replace=1
  [ "\$a" = "-l" ] && seen_list=1
done
if [ "\$seen_replace\$seen_list" = "11" ]; then
  n=0
  [ -f "\$COUNT" ] && n=\$(cat "\$COUNT")
  n=\$((n + 1))
  printf '%s' "\$n" > "\$COUNT"
  [ "\$n" = "2" ] && exit 129
fi
exec "\$REALGIT" "\$@"
SHIM
        chmod +x "$G_SHIM_LFAIL/git"
        (cd "$GATE_REPO" && PATH="$G_SHIM_LFAIL:$PATH" bash "$GATE_IN_CLONE" \
            --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22dh.log" 2>&1)
        G_RC=$?
        if [ ! -f "$G_LFAIL_COUNT" ] || [ "$(cat "$G_LFAIL_COUNT" 2>/dev/null)" -lt 2 ]; then
            t_fail "identity gate: the listing never got re-read, so 22dh would pass vacuously"
        elif [ "$G_RC" -eq 3 ] \
             && grep -q 'could not re-read the repository' "$TMP_DIR/gate-22dh.log" \
             && ! grep -q 'changed while this gate was working' "$TMP_DIR/gate-22dh.log"; then
            t_pass "identity gate: a failed metadata re-read is unanswerable, not an observed change"
        else
            t_fail "identity gate: a failed probe was reported as an observed change (rc=$G_RC; see $TMP_DIR/gate-22dh.log)"
        fi

        # 22di: THE BATCH SHARES ONE BUDGET. `remaining` floors to 1s, which is
        # right for a single call and wrong for a loop: every probe was granted
        # a fresh second however long the batch had already run, so a stalling
        # revision search overran the gate deadline in proportion to the number
        # of probes -- measured at 6.19s against a 1s budget (Codex perf,
        # section 52 round 13, [medium]). `budget_left` reports 0 rather than 1,
        # and no probe spawns past it.
        #
        # ASSERTED BY COUNTING PROBES, NOT BY THE CLOCK. A wall-clock bound is
        # both flaky and blunt: the first draft stalled every `^{commit}` call,
        # so the gate died at the HEAD resolution and the capture batch -- the
        # actual subject -- never ran, and the mutation that restores the floor
        # passed it. The stall is scoped to a spec that is CAPTURED but never
        # RESOLVED (a last-gated SHA under an explicit `--base`), so the three
        # capture probes are the only ones that reach it: one spawn is the fix,
        # three is the defect.
        G_STALL_LOG="$TMP_DIR/gate-22di-spawns"
        rm -f "$G_STALL_LOG"
        (cd "$GATE_REPO" && git branch -f s52-stall-branch "$G_PROTO_BASE" >/dev/null 2>&1)
        (cd "$GATE_REPO" && git replace -f "$G_S52_ANC" "$G_PROTO_BASE" >/dev/null 2>&1)
        G_STALL_SHIM="$TMP_DIR/gitshim-stall-s52"
        mkdir -p "$G_STALL_SHIM"
        cat > "$G_STALL_SHIM/git" <<SHIM
#!/usr/bin/env bash
SPAWNLOG=$G_STALL_LOG
REALGIT=$G_REAL_GIT
for a in "\$@"; do
  case "\$a" in
    *s52-stall-branch*)
      printf 'spawn\n' >> "\$SPAWNLOG"
      # A CHILD, and this probe's output IS CAPTURED, which is the combination
      # that matters. `timeout --foreground` explicitly does not signal the
      # command's children, so a descendant holding the command substitution's
      # stdout keeps the caller blocked for the whole stall even though the
      # bound fired -- 20s against a 2s budget. Without that flag the process
      # GROUP is signalled and the caller returns at the deadline. Git spawns
      # such descendants (hooks, promisor helpers), so this is the real shape;
      # an `exec`ed stall could never show it (Codex adversarial, section 52
      # round 29, [medium]).
      sleep 20 &
      wait
      exit 0 ;;
  esac
done
exec "\$REALGIT" "\$@"
SHIM
        chmod +x "$G_STALL_SHIM/git"
        G_T0=$SECONDS
        (cd "$GATE_REPO" && IDENTITY_GATE_BUDGET_SECS=2 \
            IDENTITY_GATE_LAST_GATED_SHA=s52-stall-branch \
            PATH="$G_STALL_SHIM:$PATH" \
            bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
            >"$TMP_DIR/gate-22di.log" 2>&1)
        G_RC=$?
        G_ELAPSED=$(( SECONDS - G_T0 ))
        G_SPAWNS=0
        [ -f "$G_STALL_LOG" ] && G_SPAWNS=$(wc -l < "$G_STALL_LOG")
        if [ "$G_SPAWNS" -eq 0 ]; then
            t_fail "identity gate: the stalling spec was never probed, so 22di would pass vacuously"
        elif [ "$G_SPAWNS" -eq 1 ] && [ "$G_RC" -ne 0 ] && [ "$G_ELAPSED" -le 8 ]; then
            t_pass "identity gate: no probe spawns once the shared budget is spent, and a stalled descendant does not outlive it (${G_SPAWNS} spawn, ${G_ELAPSED}s)"
        else
            t_fail "identity gate: the probe batch kept spawning past its budget (${G_SPAWNS} spawns, rc=$G_RC, ${G_ELAPSED}s; see $TMP_DIR/gate-22di.log)"
        fi
        (cd "$GATE_REPO" && git branch -D s52-stall-branch >/dev/null 2>&1)

        # 22dj: A CALLER ERROR IS AN ANSWER, NOT FAILED MACHINERY. The endpoint
        # probes were routed through a bounded helper whose only "does not
        # resolve" status is rc 1 -- but `git rev-parse --verify` WITHOUT
        # `--quiet` exits 128 on an invalid revision, so a plain
        # `--head not-a-ref` was classified as a probe the gate could not ask
        # and reported under ENDPOINT_UNRESOLVABLE, sending the operator to
        # repair infrastructure for their own typo (Codex adversarial, section
        # 52 round 14, [medium]). That distinction is the whole reason the
        # token was published.
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --head s52-definitely-not-a-ref \
            >"$TMP_DIR/gate-22dj.log" 2>&1)
        G_RC=$?
        if [ "$G_RC" -eq 3 ] \
           && grep -q "is not a commit" "$TMP_DIR/gate-22dj.log" \
           && ! grep -q 'ENDPOINT_UNRESOLVABLE' "$TMP_DIR/gate-22dj.log"; then
            t_pass "identity gate: an invalid head argument is answered, not blamed on machinery"
        else
            t_fail "identity gate: a caller error was reported as failed machinery (rc=$G_RC; see $TMP_DIR/gate-22dj.log)"
        fi

        # 22dk: THE FALLBACK CHAIN IS WHERE A COLLAPSED STATUS COSTS MOST. These
        # probes CHOOSE WHICH RANGE IS ADJUDICATED, so a stalled or failed
        # event-base lookup read as "that candidate is unusable" and the chain
        # advanced to HEAD~1 -- adjudicating only the last commit of a
        # multi-commit push, and able to return green over resolver changes it
        # never compared (Codex adversarial AND consistency, section 52 round
        # 14, [high]). 22di cannot see this: it supplies an explicit base, so
        # the chain is never entered.
        G_FB_LOG="$TMP_DIR/gate-22dk-spawns"
        rm -f "$G_FB_LOG"
        G_SHIM_FB="$TMP_DIR/gitshim-fallback-s52"
        mkdir -p "$G_SHIM_FB"
        cat > "$G_SHIM_FB/git" <<SHIM
#!/usr/bin/env bash
SPAWNLOG=$G_FB_LOG
REALGIT=$G_REAL_GIT
for a in "\$@"; do
  case "\$a" in
    # A PROBE FAILURE, NOT A STALL. The stall shape cannot discriminate here:
    # both the fixed and the collapsed forms end up refusing on an exhausted
    # budget, just at different points, so the mutation passed. rc 128 is what
    # a degraded git actually returns, and it separates the two cleanly --
    # tri-state refuses, a boolean falls through to HEAD~1.
    *s52-event-base*) printf 'spawn\n' >> "\$SPAWNLOG"; exit 128 ;;
  esac
done
exec "\$REALGIT" "\$@"
SHIM
        chmod +x "$G_SHIM_FB/git"
        (cd "$GATE_REPO" && git branch -f s52-event-base "$G_PROTO_BASE" >/dev/null 2>&1)
        G_FB_T0=$SECONDS
        (cd "$GATE_REPO" && env -u IDENTITY_GATE_LAST_GATED_SHA \
            IDENTITY_GATE_BUDGET_SECS=2 GITHUB_EVENT_BEFORE=s52-event-base \
            PATH="$G_SHIM_FB:$PATH" bash "$GATE_IN_CLONE" --head HEAD \
            >"$TMP_DIR/gate-22dk.log" 2>&1)
        G_RC=$?
        G_FB_ELAPSED=$(( SECONDS - G_FB_T0 ))
        if [ ! -s "$G_FB_LOG" ]; then
            t_fail "identity gate: the event-base candidate was never probed, so 22dk would pass vacuously"
        elif [ "$G_RC" -eq 3 ] \
             && grep -q 'ENDPOINT_UNRESOLVABLE' "$TMP_DIR/gate-22dk.log" \
             && ! grep -q 'falling back to HEAD~1' "$TMP_DIR/gate-22dk.log" \
             && ! grep -q '^\[identity-gate\] base ' "$TMP_DIR/gate-22dk.log"; then
            t_pass "identity gate: an unanswerable event-base refuses instead of narrowing the range"
        else
            t_fail "identity gate: a failed candidate probe silently narrowed the range or escaped the budget (rc=$G_RC, ${G_FB_ELAPSED}s; see $TMP_DIR/gate-22dk.log)"
        fi
        (cd "$GATE_REPO" && git branch -D s52-event-base >/dev/null 2>&1)

        # 22dl: THE REACHABILITY PHASE ANSWERS TO THE SAME DEADLINE. The peel
        # loop was bounded a round earlier while this phase kept an unbounded
        # `merge-base` and a walk floored to one second -- so a degraded commit
        # graph could wedge the gate here, AFTER it already knew the head was
        # substituted, suppressing a verdict it had reached (Codex adversarial,
        # section 52 round 15, [medium]). Asserted on elapsed, which is the only
        # discriminator available: the probe's status is treated identically
        # whether it is killed or answers, by design.
        G_MB_LOG="$TMP_DIR/gate-22dl-spawns"
        rm -f "$G_MB_LOG"
        G_SHIM_MB="$TMP_DIR/gitshim-mergebase-s52"
        mkdir -p "$G_SHIM_MB"
        cat > "$G_SHIM_MB/git" <<SHIM
#!/usr/bin/env bash
SPAWNLOG=$G_MB_LOG
REALGIT=$G_REAL_GIT
for a in "\$@"; do
  # DELIBERATELY TERM-RESISTANT. A cooperative `sleep` dies on the first
  # signal, so it cannot tell a hard deadline from a TERM plus a ten-second
  # grace -- and the grace was the defect: a non-cooperative probe spent it in
  # full, 12s against a 2s budget (Codex perf, section 52 round 16, [medium]).
  # A CHILD, NOT `exec`. `timeout --foreground` explicitly does not signal the
  # command's children, so a descendant that outlives the kill holds the caller
  # open past the budget -- git spawns hooks and promisor helpers, so this is
  # the shape that matters, and `exec sleep` could never show it (Codex
  # adversarial, section 52 round 29, [medium]).
  [ "\$a" = "merge-base" ] && { printf 'spawn\n' >> "\$SPAWNLOG"; trap '' TERM INT; sleep 20 & wait; exit 0; }
done
exec "\$REALGIT" "\$@"
SHIM
        chmod +x "$G_SHIM_MB/git"
        (cd "$GATE_REPO" && git replace -f "$G_S52_HEAD" "$G_PROTO_BASE" >/dev/null 2>&1)
        G_MB_T0=$SECONDS
        (cd "$GATE_REPO" && IDENTITY_GATE_BUDGET_SECS=2 PATH="$G_SHIM_MB:$PATH" \
            bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
            >"$TMP_DIR/gate-22dl.log" 2>&1)
        G_RC=$?
        G_MB_ELAPSED=$(( SECONDS - G_MB_T0 ))
        if [ ! -s "$G_MB_LOG" ]; then
            t_fail "identity gate: the ancestry probe never ran, so 22dl would pass vacuously"
        elif [ "$G_RC" -ne 0 ] && [ "$G_MB_ELAPSED" -le 5 ]; then
            t_pass "identity gate: the reachability phase stays inside the shared budget (${G_MB_ELAPSED}s)"
        else
            t_fail "identity gate: a reachability probe escaped the shared budget (rc=$G_RC, ${G_MB_ELAPSED}s; see $TMP_DIR/gate-22dl.log)"
        fi
        (cd "$GATE_REPO" && git replace -d "$G_S52_HEAD" >/dev/null 2>&1)

        # 22dm: REPLACEMENT SUPPORT CAN BE TURNED ON WHILE THE GATE RUNS. With
        # `core.useReplaceRefs` initially false the capture is skipped, the
        # listing is never read, and the revalidation used to be guarded on the
        # enabled state -- so a concurrent enable left ordinary git resolving
        # through pre-existing replace refs while the gate stayed pinned to
        # stored objects, reported clean, and advanced its baseline over a
        # substituted endpoint (Codex adversarial, section 52 round 16, [high]).
        # The same shape as the zero-to-one listing race, one level up, and
        # 22dg cannot see it because that fixture starts enabled.
        G_CFG_MARK="$TMP_DIR/gate-22dm-fired"
        rm -f "$G_CFG_MARK"
        (cd "$GATE_REPO" && git config core.useReplaceRefs false >/dev/null 2>&1)
        (cd "$GATE_REPO" && git replace -f "$G_S52_ANC" "$G_PROTO_BASE" >/dev/null 2>&1)
        G_SHIM_CFG="$TMP_DIR/gitshim-enable-s52"
        mkdir -p "$G_SHIM_CFG"
        cat > "$G_SHIM_CFG/git" <<SHIM
#!/usr/bin/env bash
MARK=$G_CFG_MARK
REALGIT=$G_REAL_GIT
for a in "\$@"; do
  if [ "\$a" = "core.useReplaceRefs" ] && [ ! -e "\$MARK" ]; then
    : > "\$MARK"
    out="\$("\$REALGIT" "\$@")"; rc=\$?
    "\$REALGIT" config core.useReplaceRefs true >/dev/null 2>&1
    [ -n "\$out" ] && printf '%s\n' "\$out"
    exit "\$rc"
  fi
done
exec "\$REALGIT" "\$@"
SHIM
        chmod +x "$G_SHIM_CFG/git"
        (cd "$GATE_REPO" && PATH="$G_SHIM_CFG:$PATH" bash "$GATE_IN_CLONE" \
            --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22dm.log" 2>&1)
        G_RC=$?
        if [ ! -e "$G_CFG_MARK" ]; then
            t_fail "identity gate: the enablement shim never fired, so 22dm would pass vacuously"
        elif [ "$G_RC" -eq 3 ] && grep -q 'core.useReplaceRefs changed' "$TMP_DIR/gate-22dm.log"; then
            t_pass "identity gate: replacement support enabled mid-run is caught, not adjudicated around"
        else
            t_fail "identity gate: a mid-run enable left the gate reporting on stale state (rc=$G_RC; see $TMP_DIR/gate-22dm.log)"
        fi
        (cd "$GATE_REPO" && git config --unset core.useReplaceRefs >/dev/null 2>&1
         cd "$GATE_REPO" && git replace -d "$G_S52_ANC" >/dev/null 2>&1)

        # 22dn: ONCE IS NOT ENOUGH. 22dm flips the setting during the CAPTURE,
        # so it only ever proved that the single mid-run revalidation sees a
        # change that happened before it. An enable landing AFTER that point
        # left the state "disabled", the listing check skipped, and the gate
        # free to exit 0 while ordinary git had already begun resolving through
        # pre-existing replace refs (Codex adversarial, section 52 round 17,
        # [high]). The state is now re-asserted immediately before every green
        # exit, and this fixture flips on the SECOND read so only that last
        # assertion can catch it.
        G_LATE_COUNT="$TMP_DIR/gate-22dn-count"
        rm -f "$G_LATE_COUNT"
        (cd "$GATE_REPO" && git config core.useReplaceRefs false >/dev/null 2>&1)
        (cd "$GATE_REPO" && git replace -f "$G_S52_ANC" "$G_PROTO_BASE" >/dev/null 2>&1)
        G_SHIM_LATE="$TMP_DIR/gitshim-lateenable-s52"
        mkdir -p "$G_SHIM_LATE"
        cat > "$G_SHIM_LATE/git" <<SHIM
#!/usr/bin/env bash
COUNT=$G_LATE_COUNT
REALGIT=$G_REAL_GIT
for a in "\$@"; do
  if [ "\$a" = "core.useReplaceRefs" ]; then
    n=0
    [ -f "\$COUNT" ] && n=\$(cat "\$COUNT")
    n=\$((n + 1))
    printf '%s' "\$n" > "\$COUNT"
    out="\$("\$REALGIT" "\$@")"; rc=\$?
    [ "\$n" = "2" ] && "\$REALGIT" config core.useReplaceRefs true >/dev/null 2>&1
    [ -n "\$out" ] && printf '%s\n' "\$out"
    exit "\$rc"
  fi
done
exec "\$REALGIT" "\$@"
SHIM
        chmod +x "$G_SHIM_LATE/git"
        (cd "$GATE_REPO" && PATH="$G_SHIM_LATE:$PATH" bash "$GATE_IN_CLONE" \
            --base "$G_S52_HEAD" --head "$G_S52_HEAD" >"$TMP_DIR/gate-22dn.log" 2>&1)
        G_RC=$?
        if [ ! -f "$G_LATE_COUNT" ] || [ "$(cat "$G_LATE_COUNT" 2>/dev/null)" -lt 3 ]; then
            t_fail "identity gate: the state was not re-asserted before the green exit, so 22dn would pass vacuously"
        elif [ "$G_RC" -eq 3 ] && grep -q 'core.useReplaceRefs changed' "$TMP_DIR/gate-22dn.log"; then
            t_pass "identity gate: a late enable is caught before the gate can return green"
        else
            t_fail "identity gate: an enable after the mid-run check still reached a green exit (rc=$G_RC; see $TMP_DIR/gate-22dn.log)"
        fi
        (cd "$GATE_REPO" && git config --unset core.useReplaceRefs >/dev/null 2>&1
         cd "$GATE_REPO" && git replace -d "$G_S52_ANC" >/dev/null 2>&1)

        # 22do: THE CLOCK PROVIDER IS PART OF THE DEADLINE. Every probe is
        # wrapped in a limit this gate computes from a subprocess clock, so an
        # interpreter that never starts stops the wrapped probe from ever
        # running -- hard-killing a child cannot help when the child is never
        # spawned (Codex adversarial AND perf, section 52 round 17, [medium],
        # found independently by both).
        G_PY_SHIM="$TMP_DIR/pyshim-clock-s52"
        mkdir -p "$G_PY_SHIM"
        cat > "$G_PY_SHIM/python3" <<'PYSHIM'
#!/usr/bin/env bash
# `exec`, so the stall IS the process the deadline is bounding. A `sleep` left
# as a CHILD outlives its parent's hard kill and keeps the command
# substitution's stdout open, so the caller blocks for the full stall even
# though the bound fired -- 30s against a 5s limit. Real interpreters have no
# such grandchild. The bound uses an unblockable signal, so no trap is needed.
exec sleep 30
PYSHIM
        chmod +x "$G_PY_SHIM/python3"
        G_CLK_T0=$SECONDS
        # A SMALL BUDGET, because the clock no longer forks. With `/proc/uptime`
        # available the wedged interpreter is met at the first bounded PYTHON
        # phase rather than at the first clock read, and that phase is bounded
        # by the gate budget -- so the budget is what this now measures.
        (cd "$GATE_REPO" && IDENTITY_GATE_BUDGET_SECS=2 PATH="$G_PY_SHIM:$PATH" \
            bash "$GATE_IN_CLONE" \
            --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22do.log" 2>&1)
        G_RC=$?
        G_CLK_ELAPSED=$(( SECONDS - G_CLK_T0 ))
        if [ "$G_RC" -ne 0 ] && [ "$G_CLK_ELAPSED" -le 8 ]; then
            t_pass "identity gate: a wedged clock provider refuses within a fixed bound (${G_CLK_ELAPSED}s)"
        else
            t_fail "identity gate: a wedged clock provider hung the gate (rc=$G_RC, ${G_CLK_ELAPSED}s; see $TMP_DIR/gate-22do.log)"
        fi

        # 22dp: AND THE BOUND IS ENFORCED STRUCTURALLY, not only observed once.
        # 22do wedges the interpreter from process start, so the FIRST bounded
        # read fails and execution never reaches the later clock sites -- which
        # is exactly how two of them stayed unbounded through a round that
        # believed the provider was covered (Codex adversarial AND perf,
        # section 52 round 18, [medium], found independently by both). A stall
        # fixture per call site would be brittle and would still miss the next
        # one added; the invariant is that the clock has ONE reader, and that
        # is checkable directly.
        G_CLK_SITES="$(grep -c 'time\.monotonic()' "$GATE_IN_CLONE" || true)"
        G_CLK_RAW="$(grep -n 'time\.monotonic()' "$GATE_IN_CLONE" \
                      | grep -v '^[0-9]*:#' | grep -vc 'python3 -c' || true)"
        # AND THE FALLBACK KEEPS ITS BOUND. The primary clock reads
        # /proc/uptime with builtins, so 22do's wedged-interpreter shim is now
        # first met by a later python phase and can no longer prove anything
        # about the fallback (Codex consistency, section 52 round 22,
        # [medium]). The bound is asserted where it lives instead.
        G_CLK_FALLBACK_BOUND="$(grep -B2 "python3 -c .import time" "$GATE_IN_CLONE" \
                                 | grep -c 'timeout .*-s KILL' || true)"
        if [ "$G_CLK_SITES" -ge 2 ] && [ "$G_CLK_RAW" -eq 0 ] \
           && [ "$(grep -c 'python3 -c .import time' "$GATE_IN_CLONE")" -eq 1 ] \
           && [ "$G_CLK_FALLBACK_BOUND" -ge 1 ]; then
            t_pass "identity gate: the monotonic clock has exactly one reader, and its fallback is bounded"
        else
            t_fail "identity gate: a clock read lives outside mono_now or its fallback is unbounded (sites=$G_CLK_SITES raw=$G_CLK_RAW bound=$G_CLK_FALLBACK_BOUND)"
        fi

        # 22dt: EVERY GIT SPAWN THIS GATE DEPENDS ON IS BOUNDED, asserted
        # structurally. A stall fixture per call site would be brittle and
        # would still miss the next site anyone adds -- which is precisely how
        # the closure passes, the type probe and the full-history traversal
        # stayed unbounded through several rounds that each believed the
        # deadline was gate-wide (Codex adversarial AND perf, section 52 rounds
        # 19-20). The invariant is checkable: a `git` invocation is either
        # wrapped in `timeout` or routed through a bounded helper. The only
        # exemption is the cleanup trap, which must run even when the budget is
        # gone -- bounding a reaper is how you leak the thing it reaps.
        G_UNBOUNDED="$(python3 - "$GATE_IN_CLONE" <<'PYGIT'
import re, sys

src = open(sys.argv[1], encoding="utf-8").read().split("\n")
# Join backslash continuations into logical lines, keeping the first line no.
logical, buf, start = [], "", 0
for i, ln in enumerate(src, 1):
    if not buf:
        start = i
    buf += ln
    if ln.rstrip().endswith("\\"):
        buf = buf.rstrip()[:-1] + " "
        continue
    logical.append((start, buf))
    buf = ""


def is_dash_c(word):
    """True for `-c` and any short-option cluster containing it: `bash -ec`
    runs its argument as a program exactly as `bash -c` does, and reading only
    the exact spellings let that form through (Codex adversarial, section 52
    round 23, [medium])."""
    return (word.startswith("-") and not word.startswith("--")
            and "c" in word[1:])


def scopes_of(line):
    """Every region of the line a shell would EXECUTE, as separate scopes.

    Substring scanning was fooled five ways (Codex adversarial, section 52
    round 22): `command git`, `env FOO=1 git`, `bash -c 'git ...'`, a backtick
    substitution, and an exemption phrase matched anywhere in the segment. A
    quoted string is a literal UNLESS it is the argument of `-c`, in which case
    it is a program; `$( )` and backticks are programs wherever they appear,
    including inside double quotes -- which is where the first state machine
    went wrong, taking the `"` that opens a nested `"$_left"` as the one that
    CLOSES the outer span and mis-tokenizing everything after it."""
    scopes, out, i, n = [], [], 0, len(line)
    dq = False
    dq_start = 0

    def last_word():
        m = re.search(r"(\S+)\s*$", "".join(out))
        return m.group(1) if m else ""

    def balanced(open_at, opener, closer):
        depth, k = 0, open_at
        while k < n:
            if line[k] == opener:
                depth += 1
            elif line[k] == closer:
                depth -= 1
                if depth == 0:
                    return k
            k += 1
        return n - 1

    while i < n:
        c = line[i]
        if c == "\\" and i + 1 < n:
            out.append("  ")
            i += 2
        elif not dq and c == "'":
            j = line.find("'", i + 1)
            j = n - 1 if j == -1 else j
            if is_dash_c(last_word()):
                scopes.extend(scopes_of(line[i + 1:j]))
            out.append(" " * (j - i + 1))
            i = j + 1
        elif c == "`":
            j = line.find("`", i + 1)
            j = n - 1 if j == -1 else j
            scopes.extend(scopes_of(line[i + 1:j]))
            out.append(" __SUBST__ ")
            i = j + 1
        elif c == "$" and i + 1 < n and line[i + 1] == "(":
            k = balanced(i + 1, "(", ")")
            scopes.extend(scopes_of(line[i + 2:k]))
            out.append(" __SUBST__ ")
            i = k + 1
        elif c == '"':
            if dq:
                if is_dash_c(last_word()):
                    scopes.extend(scopes_of(line[dq_start:i]))
                dq = False
                # A PLACEHOLDER, NOT A BLANK. Blanked to whitespace, a quoted
                # ARGUMENT vanished -- and `timeout ... "$_left" git ...` then
                # looked like `timeout ... git ...`, so the duration token the
                # checker examined was the word `git` itself.
                out.append(" __ARG__ " if i > dq_start else " ")
            else:
                dq = True
                dq_start = i + 1
                out.append(" ")
            i += 1
        elif c == "#" and not dq and (not out or out[-1] in " \t;&|("):
            break
        else:
            out.append(" " if dq else c)
            i += 1
    scopes.insert(0, "".join(out))
    return scopes


# Grouping and negation are not commands; they prefix one. `{ git ...; }` and
# `! git ...` executed git while the scanner read the brace or the bang as the
# command word (Codex adversarial, section 52 round 23, [medium]).
SEGMENT_SPLIT = re.compile(r"[;&|(){}]+|\bthen\b|\bdo\b|\belse\b|\bif\b|\bwhile\b|\bfi\b|\bdone\b")
ENV_ASSIGN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=\S*\s+")
# Prefixes that execute the REST of the segment and so must be unwrapped, not
# accepted as the command word. `--` ends a wrapper's own options.
WRAPPERS = re.compile(r"^(?:command|builtin|exec|nohup|setsid|time|eval)\s+(?:--\s+)?")
# `-i` TAKES NO OPERAND AND `-u` DOES, and treating them alike made the regex
# swallow the command itself: `env -i git rev-parse HEAD` consumed `env -i git`
# and left `rev-parse HEAD`, so the scanner saw no spawn at all (Codex
# adversarial, section 52 round 27, [medium]).
ENV_CMD = re.compile(r"^env\s+(?:-i\s+|-u\s+\S+\s+|--\s+|[A-Za-z_][A-Za-z0-9_]*=\S*\s+)*")
NEGATION = re.compile(r"^!\s+")
# A duration of literally 0 DISABLES timeout(1) -- the same trap this gate
# already documents for its own budget floor. A wrapper that disables itself is
# not a bound, so the scanner refuses to accept it as one.
# OPTIONS WITH OPERANDS, PARSED WITH THEIR REAL ARITY. Treating every short
# option as operand-free made `timeout -s KILL 0 git ...` read `KILL` as the
# duration and leave the zero behind -- and `-s KILL` is the form this gate
# uses everywhere, so that is precisely where a disabling duration would hide
# (Codex adversarial, section 52 round 28, [medium]).
TIMEOUT_CMD = re.compile(
    r"^timeout\s+("
    r"(?:"
    r"(?:-[sk]\s+\S+|--(?:signal|kill-after)(?:=\S+|\s+\S+))\s+"
    r"|--[a-z-]+(?:=\S+)?\s+"
    r"|-[a-zA-Z]+\s+"
    r")*"
    r")(\S+)\s+")


# ONLY A NARROW FINITE-POSITIVE DECIMAL COUNTS AS A BOUND. Asking "does this
# parse to zero" was still too generous: `timeout 0x0` and `timeout inf` both
# run forever, and `float()` rejects the first and returns infinity for the
# second, so neither read as zero and both passed as bounded (Codex
# adversarial, section 52 round 26, [medium]). The question is inverted -- a
# duration must PROVE it bounds something.
_DURATION = re.compile(r"^\+?(?:\d+(?:\.\d*)?|\.\d+)[smhd]?$")


def _is_zero_duration(word):
    """True when this duration does NOT bound anything: zero, or a spelling
    outside the grammar we are prepared to vouch for."""
    # A COMPUTED duration is accepted: the code that writes one guards it
    # against zero before it spawns, and this scan cannot evaluate it. Only a
    # LITERAL has to prove itself here.
    if word == "__ARG__" or word.startswith("$"):
        return False
    if not _DURATION.match(word):
        return True
    body = word[:-1] if word[-1:] in "smhd" else word
    try:
        v = float(body)
    except ValueError:
        return True
    return not (v > 0.0) or v != v or v in (float("inf"),)


def normalized(segment):
    """The segment with execution prefixes, negation and env assignments
    removed -- and with a self-disabling `timeout 0` deliberately NOT removed,
    so it reads as the unbounded spawn it is."""
    seg = segment.strip()
    changed = True
    while changed:
        changed = False
        for pat in (ENV_ASSIGN, WRAPPERS, ENV_CMD, NEGATION):
            m = pat.match(seg)
            if m and m.end() > 0:
                seg = seg[m.end():].lstrip()
                changed = True
        m = TIMEOUT_CMD.match(seg)
        # EVERY SPELLING OF ZERO, parsed as a NUMBER rather than matched as a
        # shape. timeout(1) takes an optional s/m/h/d suffix and a full
        # floating-point grammar, so `0`, `0s`, `00`, `.0`, `0e0`, `0E+0` and
        # `+0` all disable it -- and a regex written for the obvious spellings
        # missed five of those (Codex adversarial, section 52 rounds 24 and
        # 25, [medium]). Anything that parses to zero is no bound at all.
        if m and _is_zero_duration(m.group(2)):
            seg = seg[m.end():].lstrip()   # a zero duration is no bound at all
            changed = True
    return seg


# EXACT command forms, anchored at the start of the normalized segment -- an
# exemption matched anywhere let `git rev-parse HEAD worktree remove` through.
EXEMPT = (
    # The cleanup trap must run even when the budget is gone; bounding a
    # reaper is how you leak the thing it reaps.
    re.compile(r"^git\s+worktree\s+(?:remove|prune)\b"),
    # Supervised as a whole pipeline by the `timeout ... bash -c` around it;
    # the body is scanned as its own scope, so the exemption is only for the
    # producer inside that already-bounded shell. Fixture 22ds proves the bound
    # by stalling the consumer.
    re.compile(r"^git\s+--no-replace-objects\s+rev-list\b"),
)

bad = []
for no, ln in logical:
    hit = None
    for scope in scopes_of(ln):
        for seg in SEGMENT_SPLIT.split(scope):
            norm = normalized(seg)
            if not re.match(r"^git(\s|$)", norm):
                continue
            if any(e.match(norm) for e in EXEMPT):
                continue
            hit = norm[:60]
            break
        if hit:
            break
    if hit:
        bad.append("%d:%s" % (no, hit))
print("\n".join(bad))
PYGIT
)"
        # THE SCANNER'S OWN SENSITIVITY IS A FIXTURE, not a claim. Five bypass
        # forms were accepted by the first two drafts -- `command git`,
        # `env FOO=1 git`, `bash -c 'git ...'`, a backtick substitution, and an
        # exemption phrase matched anywhere in the segment (Codex adversarial,
        # section 52 round 22, [medium]) -- and five MORE by the third:
        # `timeout 0 git ...`, where a zero duration disables timeout(1)
        # outright, `command -- git`, `{ git ...; }`, `! git ...` and
        # `bash -ec 'git ...'` (round 23, [medium]), and three more spellings
        # of a zero duration in round 24 and five more in round 25, once the
        # duration was parsed as a NUMBER rather than matched as a shape, and
        # three more in round 26 once the question was inverted again: a
        # duration must PROVE it bounds something, because `0x0` and `inf` are
        # neither zero nor a bound. A
        # scan that cannot be
        # shown to FIND them proves nothing when it reports a clean file, so
        # every one of them is re-detected on each run.
        G_CX="$TMP_DIR/gate-22dt-counterexamples.sh"
        cat > "$G_CX" <<'CXEOF'
command git rev-parse HEAD
env FOO=1 git rev-parse HEAD
bash -c 'git rev-parse HEAD'
_x=`git rev-parse HEAD`
git rev-parse HEAD worktree remove
timeout 1 true; git rev-parse HEAD
_y="$(git ls-tree HEAD -- p)"   # worktree remove
timeout 0 git rev-parse HEAD
command -- git rev-parse HEAD
{ git rev-parse HEAD; }
! git rev-parse HEAD
bash -ec 'git rev-parse HEAD'
timeout 0s git rev-parse HEAD
timeout 0.0 git rev-parse HEAD
timeout --preserve-status 00 git rev-parse HEAD
timeout .0 git rev-parse HEAD
timeout .0s git rev-parse HEAD
timeout 0e0 git rev-parse HEAD
timeout 0E+0 git rev-parse HEAD
timeout +0 git rev-parse HEAD
timeout 0x0 git rev-parse HEAD
timeout inf git rev-parse HEAD
timeout 1e309 git rev-parse HEAD
env -i git rev-parse HEAD
timeout -s KILL 0 git rev-parse HEAD
CXEOF
        G_CX_HITS="$(python3 - "$G_CX" <<'PYCX'
import re, sys

src = open(sys.argv[1], encoding="utf-8").read().split("\n")
# Join backslash continuations into logical lines, keeping the first line no.
logical, buf, start = [], "", 0
for i, ln in enumerate(src, 1):
    if not buf:
        start = i
    buf += ln
    if ln.rstrip().endswith("\\"):
        buf = buf.rstrip()[:-1] + " "
        continue
    logical.append((start, buf))
    buf = ""


def is_dash_c(word):
    """True for `-c` and any short-option cluster containing it: `bash -ec`
    runs its argument as a program exactly as `bash -c` does, and reading only
    the exact spellings let that form through (Codex adversarial, section 52
    round 23, [medium])."""
    return (word.startswith("-") and not word.startswith("--")
            and "c" in word[1:])


def scopes_of(line):
    """Every region of the line a shell would EXECUTE, as separate scopes.

    Substring scanning was fooled five ways (Codex adversarial, section 52
    round 22): `command git`, `env FOO=1 git`, `bash -c 'git ...'`, a backtick
    substitution, and an exemption phrase matched anywhere in the segment. A
    quoted string is a literal UNLESS it is the argument of `-c`, in which case
    it is a program; `$( )` and backticks are programs wherever they appear,
    including inside double quotes -- which is where the first state machine
    went wrong, taking the `"` that opens a nested `"$_left"` as the one that
    CLOSES the outer span and mis-tokenizing everything after it."""
    scopes, out, i, n = [], [], 0, len(line)
    dq = False
    dq_start = 0

    def last_word():
        m = re.search(r"(\S+)\s*$", "".join(out))
        return m.group(1) if m else ""

    def balanced(open_at, opener, closer):
        depth, k = 0, open_at
        while k < n:
            if line[k] == opener:
                depth += 1
            elif line[k] == closer:
                depth -= 1
                if depth == 0:
                    return k
            k += 1
        return n - 1

    while i < n:
        c = line[i]
        if c == "\\" and i + 1 < n:
            out.append("  ")
            i += 2
        elif not dq and c == "'":
            j = line.find("'", i + 1)
            j = n - 1 if j == -1 else j
            if is_dash_c(last_word()):
                scopes.extend(scopes_of(line[i + 1:j]))
            out.append(" " * (j - i + 1))
            i = j + 1
        elif c == "`":
            j = line.find("`", i + 1)
            j = n - 1 if j == -1 else j
            scopes.extend(scopes_of(line[i + 1:j]))
            out.append(" __SUBST__ ")
            i = j + 1
        elif c == "$" and i + 1 < n and line[i + 1] == "(":
            k = balanced(i + 1, "(", ")")
            scopes.extend(scopes_of(line[i + 2:k]))
            out.append(" __SUBST__ ")
            i = k + 1
        elif c == '"':
            if dq:
                if is_dash_c(last_word()):
                    scopes.extend(scopes_of(line[dq_start:i]))
                dq = False
                # A PLACEHOLDER, NOT A BLANK. Blanked to whitespace, a quoted
                # ARGUMENT vanished -- and `timeout ... "$_left" git ...` then
                # looked like `timeout ... git ...`, so the duration token the
                # checker examined was the word `git` itself.
                out.append(" __ARG__ " if i > dq_start else " ")
            else:
                dq = True
                dq_start = i + 1
                out.append(" ")
            i += 1
        elif c == "#" and not dq and (not out or out[-1] in " \t;&|("):
            break
        else:
            out.append(" " if dq else c)
            i += 1
    scopes.insert(0, "".join(out))
    return scopes


# Grouping and negation are not commands; they prefix one. `{ git ...; }` and
# `! git ...` executed git while the scanner read the brace or the bang as the
# command word (Codex adversarial, section 52 round 23, [medium]).
SEGMENT_SPLIT = re.compile(r"[;&|(){}]+|\bthen\b|\bdo\b|\belse\b|\bif\b|\bwhile\b|\bfi\b|\bdone\b")
ENV_ASSIGN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=\S*\s+")
# Prefixes that execute the REST of the segment and so must be unwrapped, not
# accepted as the command word. `--` ends a wrapper's own options.
WRAPPERS = re.compile(r"^(?:command|builtin|exec|nohup|setsid|time|eval)\s+(?:--\s+)?")
# `-i` TAKES NO OPERAND AND `-u` DOES, and treating them alike made the regex
# swallow the command itself: `env -i git rev-parse HEAD` consumed `env -i git`
# and left `rev-parse HEAD`, so the scanner saw no spawn at all (Codex
# adversarial, section 52 round 27, [medium]).
ENV_CMD = re.compile(r"^env\s+(?:-i\s+|-u\s+\S+\s+|--\s+|[A-Za-z_][A-Za-z0-9_]*=\S*\s+)*")
NEGATION = re.compile(r"^!\s+")
# A duration of literally 0 DISABLES timeout(1) -- the same trap this gate
# already documents for its own budget floor. A wrapper that disables itself is
# not a bound, so the scanner refuses to accept it as one.
# OPTIONS WITH OPERANDS, PARSED WITH THEIR REAL ARITY. Treating every short
# option as operand-free made `timeout -s KILL 0 git ...` read `KILL` as the
# duration and leave the zero behind -- and `-s KILL` is the form this gate
# uses everywhere, so that is precisely where a disabling duration would hide
# (Codex adversarial, section 52 round 28, [medium]).
TIMEOUT_CMD = re.compile(
    r"^timeout\s+("
    r"(?:"
    r"(?:-[sk]\s+\S+|--(?:signal|kill-after)(?:=\S+|\s+\S+))\s+"
    r"|--[a-z-]+(?:=\S+)?\s+"
    r"|-[a-zA-Z]+\s+"
    r")*"
    r")(\S+)\s+")


# ONLY A NARROW FINITE-POSITIVE DECIMAL COUNTS AS A BOUND. Asking "does this
# parse to zero" was still too generous: `timeout 0x0` and `timeout inf` both
# run forever, and `float()` rejects the first and returns infinity for the
# second, so neither read as zero and both passed as bounded (Codex
# adversarial, section 52 round 26, [medium]). The question is inverted -- a
# duration must PROVE it bounds something.
_DURATION = re.compile(r"^\+?(?:\d+(?:\.\d*)?|\.\d+)[smhd]?$")


def _is_zero_duration(word):
    """True when this duration does NOT bound anything: zero, or a spelling
    outside the grammar we are prepared to vouch for."""
    # A COMPUTED duration is accepted: the code that writes one guards it
    # against zero before it spawns, and this scan cannot evaluate it. Only a
    # LITERAL has to prove itself here.
    if word == "__ARG__" or word.startswith("$"):
        return False
    if not _DURATION.match(word):
        return True
    body = word[:-1] if word[-1:] in "smhd" else word
    try:
        v = float(body)
    except ValueError:
        return True
    return not (v > 0.0) or v != v or v in (float("inf"),)


def normalized(segment):
    """The segment with execution prefixes, negation and env assignments
    removed -- and with a self-disabling `timeout 0` deliberately NOT removed,
    so it reads as the unbounded spawn it is."""
    seg = segment.strip()
    changed = True
    while changed:
        changed = False
        for pat in (ENV_ASSIGN, WRAPPERS, ENV_CMD, NEGATION):
            m = pat.match(seg)
            if m and m.end() > 0:
                seg = seg[m.end():].lstrip()
                changed = True
        m = TIMEOUT_CMD.match(seg)
        # EVERY SPELLING OF ZERO, parsed as a NUMBER rather than matched as a
        # shape. timeout(1) takes an optional s/m/h/d suffix and a full
        # floating-point grammar, so `0`, `0s`, `00`, `.0`, `0e0`, `0E+0` and
        # `+0` all disable it -- and a regex written for the obvious spellings
        # missed five of those (Codex adversarial, section 52 rounds 24 and
        # 25, [medium]). Anything that parses to zero is no bound at all.
        if m and _is_zero_duration(m.group(2)):
            seg = seg[m.end():].lstrip()   # a zero duration is no bound at all
            changed = True
    return seg


# EXACT command forms, anchored at the start of the normalized segment -- an
# exemption matched anywhere let `git rev-parse HEAD worktree remove` through.
EXEMPT = (
    # The cleanup trap must run even when the budget is gone; bounding a
    # reaper is how you leak the thing it reaps.
    re.compile(r"^git\s+worktree\s+(?:remove|prune)\b"),
    # Supervised as a whole pipeline by the `timeout ... bash -c` around it;
    # the body is scanned as its own scope, so the exemption is only for the
    # producer inside that already-bounded shell. Fixture 22ds proves the bound
    # by stalling the consumer.
    re.compile(r"^git\s+--no-replace-objects\s+rev-list\b"),
)

bad = []
for no, ln in logical:
    hit = None
    for scope in scopes_of(ln):
        for seg in SEGMENT_SPLIT.split(scope):
            norm = normalized(seg)
            if not re.match(r"^git(\s|$)", norm):
                continue
            if any(e.match(norm) for e in EXEMPT):
                continue
            hit = norm[:60]
            break
        if hit:
            break
    if hit:
        bad.append("%d:%s" % (no, hit))
print("\n".join(bad))
PYCX
)"
        G_CX_N="$(printf '%s\n' "$G_CX_HITS" | grep -c . || true)"
        if [ "$G_CX_N" -ne 25 ]; then
            t_fail "identity gate: the spawn scanner missed $((25 - G_CX_N)) of 25 known bypass forms, so a clean report means nothing"
        elif [ -z "$G_UNBOUNDED" ]; then
            t_pass "identity gate: every git spawn is bounded by the shared deadline (scanner sees all 25 bypass forms)"
        else
            t_fail "identity gate: unbounded git spawn(s): $(printf '%s' "$G_UNBOUNDED" | tr '\n' ' ')"
        fi

        # 22du: THE ORDINARY REPOSITORY HAS NO REPLACEMENTS, and that is where
        # the endpoint freshness check has to hold. The peel array is populated
        # only when replacement metadata exists, so on a normal repository
        # every pre-exit assertion iterated an EMPTY array and a branch-valued
        # endpoint could advance after resolution while the gate returned zero
        # about the old commit (Codex adversarial, section 52 round 20,
        # [high]). 22dq cannot see this: it installs a replacement first, which
        # is exactly what makes the array non-empty -- and disabling the
        # unconditional check left 22dq green, which is how this gap was found.
        G_ADV2_COUNT="$TMP_DIR/gate-22du-count"
        rm -f "$G_ADV2_COUNT"
        (cd "$GATE_REPO" && git branch -f s52-adv2-branch "$G_PROTO_BASE" >/dev/null 2>&1)
        if [ -n "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
            t_fail "identity gate: 22du needs a repository with no replacements"
        else
            G_SHIM_ADV2="$TMP_DIR/gitshim-advance2-s52"
            mkdir -p "$G_SHIM_ADV2"
            cat > "$G_SHIM_ADV2/git" <<SHIM
#!/usr/bin/env bash
COUNT=$G_ADV2_COUNT
REALGIT=$G_REAL_GIT
TARGET=$G_CORRUPT
# With no replacement metadata the state assertion's only spawn is this config
# read, so counting it identifies each assertion. The first is mid-run, the
# second guards the green exit.
for a in "\$@"; do
  if [ "\$a" = "core.useReplaceRefs" ]; then
    n=0
    [ -f "\$COUNT" ] && n=\$(cat "\$COUNT")
    n=\$((n + 1))
    printf '%s' "\$n" > "\$COUNT"
    if [ "\$n" = "2" ]; then
      out="\$("\$REALGIT" "\$@")"; rc=\$?
      "\$REALGIT" branch -f s52-adv2-branch "\$TARGET" >/dev/null 2>&1
      [ -n "\$out" ] && printf '%s\n' "\$out"
      exit "\$rc"
    fi
  fi
done
exec "\$REALGIT" "\$@"
SHIM
            chmod +x "$G_SHIM_ADV2/git"
            (cd "$GATE_REPO" && PATH="$G_SHIM_ADV2:$PATH" bash "$GATE_IN_CLONE" \
                --base "$G_PROTO_BASE" --head s52-adv2-branch >"$TMP_DIR/gate-22du.log" 2>&1)
            G_RC=$?
            if [ ! -f "$G_ADV2_COUNT" ] || [ "$(cat "$G_ADV2_COUNT" 2>/dev/null)" -lt 2 ]; then
                t_fail "identity gate: the state was not re-asserted on the no-replacement path, so 22du would pass vacuously"
            elif [ "$G_RC" -eq 3 ] && grep -q 'no longer resolves to' "$TMP_DIR/gate-22du.log"; then
                t_pass "identity gate: endpoint freshness holds on a repository with no replacements"
            else
                t_fail "identity gate: a late endpoint advance reached a green exit with no replacements (rc=$G_RC; see $TMP_DIR/gate-22du.log)"
            fi
            (cd "$GATE_REPO" && git branch -D s52-adv2-branch >/dev/null 2>&1)
        fi

        # 22dv: THE OTHER DIRECTION OF THE UNREADABLE SETTING. 22dr fails the
        # RE-READ; failing the CAPTURE instead left the comparison emitting
        # "changed from '<unreadable>'" -- movement asserted from a value that
        # was never observed (Codex consistency, section 52 round 20, [medium]).
        G_CFGFAIL1="$TMP_DIR/gate-22dv-count"
        rm -f "$G_CFGFAIL1"
        G_SHIM_CFG1="$TMP_DIR/gitshim-cfgfail1-s52"
        mkdir -p "$G_SHIM_CFG1"
        cat > "$G_SHIM_CFG1/git" <<SHIM
#!/usr/bin/env bash
COUNT=$G_CFGFAIL1
REALGIT=$G_REAL_GIT
for a in "\$@"; do
  if [ "\$a" = "core.useReplaceRefs" ]; then
    n=0
    [ -f "\$COUNT" ] && n=\$(cat "\$COUNT")
    n=\$((n + 1))
    printf '%s' "\$n" > "\$COUNT"
    [ "\$n" = "1" ] && exit 129
  fi
done
exec "\$REALGIT" "\$@"
SHIM
        chmod +x "$G_SHIM_CFG1/git"
        (cd "$GATE_REPO" && PATH="$G_SHIM_CFG1:$PATH" bash "$GATE_IN_CLONE" \
            --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22dv.log" 2>&1)
        G_RC=$?
        if [ ! -f "$G_CFGFAIL1" ]; then
            t_fail "identity gate: the setting was never read, so 22dv would pass vacuously"
        elif [ "$G_RC" -eq 3 ] \
             && grep -q 'could not read core.useReplaceRefs on at least one side' "$TMP_DIR/gate-22dv.log" \
             && ! grep -q 'core.useReplaceRefs changed' "$TMP_DIR/gate-22dv.log"; then
            t_pass "identity gate: an unreadable CAPTURE is not reported as a change either"
        else
            t_fail "identity gate: a failed capture probe was reported as a change (rc=$G_RC; see $TMP_DIR/gate-22dv.log)"
        fi

        # 22dw / 22dx: ENDPOINT FRESHNESS IS NOT A REPLACEMENT QUESTION, so it
        # cannot sit behind the replacement-state gate. Placed after it,
        # "unconditional" held for exactly one of the three states: with
        # `core.useReplaceRefs=false`, or with the operator's own documented
        # `GIT_NO_REPLACE_OBJECTS` opt-out, the assertion returned before ever
        # reaching the endpoint loop, so a branch could advance and the gate
        # certified a range it was not asked about (Codex adversarial, section
        # 52 round 21, [high]). 22du covers only the enabled state.
        for G_STATE in disabled optout; do
            G_FR_COUNT="$TMP_DIR/gate-22dw-$G_STATE-count"
            rm -f "$G_FR_COUNT"
            (cd "$GATE_REPO" && git branch -f s52-fresh-branch "$G_PROTO_BASE" >/dev/null 2>&1)
            if [ "$G_STATE" = "disabled" ]; then
                (cd "$GATE_REPO" && git config core.useReplaceRefs false >/dev/null 2>&1)
            else
                (cd "$GATE_REPO" && git config --unset core.useReplaceRefs >/dev/null 2>&1)
            fi
            G_SHIM_FR="$TMP_DIR/gitshim-fresh-$G_STATE-s52"
            mkdir -p "$G_SHIM_FR"
            cat > "$G_SHIM_FR/git" <<SHIM
#!/usr/bin/env bash
COUNT=$G_FR_COUNT
REALGIT=$G_REAL_GIT
TARGET=$G_CORRUPT
for a in "\$@"; do
  if [ "\$a" = "core.useReplaceRefs" ]; then
    n=0
    [ -f "\$COUNT" ] && n=\$(cat "\$COUNT")
    n=\$((n + 1))
    printf '%s' "\$n" > "\$COUNT"
    if [ "\$n" = "2" ]; then
      out="\$("\$REALGIT" "\$@")"; rc=\$?
      "\$REALGIT" branch -f s52-fresh-branch "\$TARGET" >/dev/null 2>&1
      [ -n "\$out" ] && printf '%s\n' "\$out"
      exit "\$rc"
    fi
  fi
done
exec "\$REALGIT" "\$@"
SHIM
            chmod +x "$G_SHIM_FR/git"
            if [ "$G_STATE" = "optout" ]; then
                (cd "$GATE_REPO" && GIT_NO_REPLACE_OBJECTS=1 PATH="$G_SHIM_FR:$PATH" \
                    bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head s52-fresh-branch \
                    >"$TMP_DIR/gate-22dw-$G_STATE.log" 2>&1)
            else
                (cd "$GATE_REPO" && PATH="$G_SHIM_FR:$PATH" \
                    bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head s52-fresh-branch \
                    >"$TMP_DIR/gate-22dw-$G_STATE.log" 2>&1)
            fi
            G_RC=$?
            if [ ! -f "$G_FR_COUNT" ] || [ "$(cat "$G_FR_COUNT" 2>/dev/null)" -lt 2 ]; then
                t_fail "identity gate: the $G_STATE state never re-asserted, so its freshness case would pass vacuously"
            elif [ "$G_RC" -eq 3 ] && grep -q 'no longer resolves to' "$TMP_DIR/gate-22dw-$G_STATE.log"; then
                t_pass "identity gate: endpoint freshness holds with replacements $G_STATE"
            else
                t_fail "identity gate: freshness was skipped with replacements $G_STATE (rc=$G_RC; see $TMP_DIR/gate-22dw-$G_STATE.log)"
            fi
            (cd "$GATE_REPO" && git branch -D s52-fresh-branch >/dev/null 2>&1
             cd "$GATE_REPO" && git config --unset core.useReplaceRefs >/dev/null 2>&1)
        done

        # 22dy: THE SETTING IS OBSERVED LAST, and that ordering is the fix.
        # Read at the TOP of the assertion and compared there, a flip landing
        # DURING the endpoint and metadata probes was invisible: those reads
        # stay pinned to stored objects, the saved values compared equal, and
        # the assertion returned success over a repository that had begun
        # substituting (Codex adversarial, section 52 round 22, [high]). This
        # fixture flips during the final assertion's LISTING read, which sits
        # between the endpoint probes and the setting read.
        G_LAST_COUNT="$TMP_DIR/gate-22dy-count"
        rm -f "$G_LAST_COUNT"
        # AN UNREACHABLE SUBJECT, so the capture runs but nothing refuses before
        # the exit this fixture needs to reach. Replacing a reachable object
        # instead made the base adjudication refuse first and the case passed
        # vacuously, which its own probe control caught.
        G_DY_EMPTY="$( (cd "$GATE_REPO" && git hash-object -t tree /dev/null 2>/dev/null) )"
        G_DY_ORPHAN="$( (cd "$GATE_REPO" && git commit-tree "$G_DY_EMPTY" -m "22dy: unreachable" </dev/null 2>/dev/null) )"
        if [ -z "$G_DY_ORPHAN" ]; then
            t_fail "identity gate: could not build the 22dy unreachable subject"
        fi
        (cd "$GATE_REPO" && git replace -f "$G_DY_ORPHAN" "$G_PROTO_BASE" >/dev/null 2>&1)
        G_SHIM_LAST="$TMP_DIR/gitshim-settinglast-s52"
        mkdir -p "$G_SHIM_LAST"
        cat > "$G_SHIM_LAST/git" <<SHIM
#!/usr/bin/env bash
COUNT=$G_LAST_COUNT
REALGIT=$G_REAL_GIT
seen_replace=0
seen_list=0
for a in "\$@"; do
  [ "\$a" = "replace" ] && seen_replace=1
  [ "\$a" = "-l" ] && seen_list=1
done
if [ "\$seen_replace\$seen_list" = "11" ]; then
  n=0
  [ -f "\$COUNT" ] && n=\$(cat "\$COUNT")
  n=\$((n + 1))
  printf '%s' "\$n" > "\$COUNT"
  if [ "\$n" = "3" ]; then
    out="\$("\$REALGIT" "\$@")"; rc=\$?
    "\$REALGIT" config core.useReplaceRefs false >/dev/null 2>&1
    [ -n "\$out" ] && printf '%s\n' "\$out"
    exit "\$rc"
  fi
fi
exec "\$REALGIT" "\$@"
SHIM
        chmod +x "$G_SHIM_LAST/git"
        (cd "$GATE_REPO" && PATH="$G_SHIM_LAST:$PATH" bash "$GATE_IN_CLONE" \
            --base "$G_S52_HEAD" --head "$G_S52_HEAD" >"$TMP_DIR/gate-22dy.log" 2>&1)
        G_RC=$?
        if [ ! -f "$G_LAST_COUNT" ] || [ "$(cat "$G_LAST_COUNT" 2>/dev/null)" -lt 3 ]; then
            t_fail "identity gate: the final assertion never re-read the listing, so 22dy would pass vacuously"
        elif [ "$G_RC" -eq 3 ] && grep -q 'core.useReplaceRefs changed' "$TMP_DIR/gate-22dy.log"; then
            t_pass "identity gate: a setting flip during the final probes is still caught"
        else
            t_fail "identity gate: the setting was compared before the probes it must outlast (rc=$G_RC; see $TMP_DIR/gate-22dy.log)"
        fi
        (cd "$GATE_REPO" && git config --unset core.useReplaceRefs >/dev/null 2>&1
         cd "$GATE_REPO" && git replace -d "$G_DY_ORPHAN" >/dev/null 2>&1)

        # 22dz: THE REPLACEMENT WALK MUST NOT FAIL OPEN. The walk is supervised
        # as one pipeline inside `bash -c`, and shell options do NOT cross that
        # boundary -- so `pipefail` was absent, the pipeline reported awk's
        # status, and a FAILED `rev-list` with no hit came back rc 0 and was
        # read as "not substituted" (Codex adversarial, section 52 round 24,
        # [high]). Of every direction this precondition can be wrong, that is
        # the only one that matters: it lets a substituted repository through.
        G_RLFAIL_SHIM="$TMP_DIR/gitshim-revlistfail-s52"
        mkdir -p "$G_RLFAIL_SHIM"
        cat > "$G_RLFAIL_SHIM/git" <<SHIM
#!/usr/bin/env bash
REALGIT=$G_REAL_GIT
for a in "\$@"; do
  [ "\$a" = "rev-list" ] && exit 128
done
exec "\$REALGIT" "\$@"
SHIM
        chmod +x "$G_RLFAIL_SHIM/git"
        (cd "$GATE_REPO" && git replace -f "$G_S52_ANC" "$G_PROTO_BASE" >/dev/null 2>&1)
        if [ -z "$( (cd "$GATE_REPO" && git replace -l 2>/dev/null) )" ]; then
            t_fail "identity gate: the walk-failure fixture has no replacement, so it would pass vacuously"
        else
            (cd "$GATE_REPO" && PATH="$G_RLFAIL_SHIM:$PATH" bash "$GATE_IN_CLONE" \
                --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22dz.log" 2>&1)
            G_RC=$?
            if [ "$G_RC" -eq 3 ] \
               && grep -q 'could not determine whether repository replacement metadata' "$TMP_DIR/gate-22dz.log"; then
                t_pass "identity gate: a failed replacement walk refuses instead of reading as clean"
            else
                t_fail "identity gate: the replacement walk failed OPEN (rc=$G_RC; see $TMP_DIR/gate-22dz.log)"
            fi
            (cd "$GATE_REPO" && git replace -d "$G_S52_ANC" >/dev/null 2>&1)
        fi

        # 22dq: THE ENDPOINT PEELS EXPIRE TOO, and asserting them once mid-run
        # was not enough. A branch can advance during the protocol, cache and
        # resolver phases, after which the gate keeps adjudicating the OLD
        # resolution and can return zero while the requested endpoint names a
        # commit it never examined (Codex adversarial, section 52 round 19,
        # [high]). The re-peel now lives inside the state assertion, which runs
        # before every green exit; this fixture moves the ref during the LAST
        # of those, so only that assertion can catch it.
        G_ADV_COUNT="$TMP_DIR/gate-22dq-count"
        rm -f "$G_ADV_COUNT"
        (cd "$GATE_REPO" && git branch -f s52-adv-branch "$G_PROTO_BASE" >/dev/null 2>&1)
        (cd "$GATE_REPO" && git replace -f "$G_S52_ANC" "$G_PROTO_BASE" >/dev/null 2>&1)
        G_SHIM_ADV="$TMP_DIR/gitshim-advance-s52"
        mkdir -p "$G_SHIM_ADV"
        cat > "$G_SHIM_ADV/git" <<SHIM
#!/usr/bin/env bash
COUNT=$G_ADV_COUNT
REALGIT=$G_REAL_GIT
TARGET=$G_CORRUPT
seen_replace=0
seen_list=0
for a in "\$@"; do
  [ "\$a" = "replace" ] && seen_replace=1
  [ "\$a" = "-l" ] && seen_list=1
done
if [ "\$seen_replace\$seen_list" = "11" ]; then
  n=0
  [ -f "\$COUNT" ] && n=\$(cat "\$COUNT")
  n=\$((n + 1))
  printf '%s' "\$n" > "\$COUNT"
  if [ "\$n" = "3" ]; then
    out="\$("\$REALGIT" "\$@")"; rc=\$?
    "\$REALGIT" branch -f s52-adv-branch "\$TARGET" >/dev/null 2>&1
    [ -n "\$out" ] && printf '%s\n' "\$out"
    exit "\$rc"
  fi
fi
exec "\$REALGIT" "\$@"
SHIM
        chmod +x "$G_SHIM_ADV/git"
        (cd "$GATE_REPO" && PATH="$G_SHIM_ADV:$PATH" bash "$GATE_IN_CLONE" \
            --base "$G_PROTO_BASE" --head s52-adv-branch >"$TMP_DIR/gate-22dq.log" 2>&1)
        G_RC=$?
        if [ ! -f "$G_ADV_COUNT" ] || [ "$(cat "$G_ADV_COUNT" 2>/dev/null)" -lt 3 ]; then
            t_fail "identity gate: the state was not re-asserted before the green exit, so 22dq would pass vacuously"
        elif [ "$G_RC" -eq 3 ] && grep -q 'no longer resolves to' "$TMP_DIR/gate-22dq.log"; then
            t_pass "identity gate: an endpoint that advanced late is caught before the gate returns green"
        else
            t_fail "identity gate: a late endpoint advance reached a green exit (rc=$G_RC; see $TMP_DIR/gate-22dq.log)"
        fi
        (cd "$GATE_REPO" && git branch -D s52-adv-branch >/dev/null 2>&1)

        # 22dr: AN UNREADABLE SETTING IS NOT AN OBSERVED CHANGE. Compared
        # directly it produced a message asserting core.useReplaceRefs had
        # CHANGED when the gate had merely failed to ask -- the same conflation
        # the listing re-read was repaired for a round earlier (Codex
        # consistency, section 52 round 19, [medium]).
        G_CFGFAIL_COUNT="$TMP_DIR/gate-22dr-count"
        rm -f "$G_CFGFAIL_COUNT"
        G_SHIM_CFGFAIL="$TMP_DIR/gitshim-cfgfail-s52"
        mkdir -p "$G_SHIM_CFGFAIL"
        cat > "$G_SHIM_CFGFAIL/git" <<SHIM
#!/usr/bin/env bash
COUNT=$G_CFGFAIL_COUNT
REALGIT=$G_REAL_GIT
for a in "\$@"; do
  if [ "\$a" = "core.useReplaceRefs" ]; then
    n=0
    [ -f "\$COUNT" ] && n=\$(cat "\$COUNT")
    n=\$((n + 1))
    printf '%s' "\$n" > "\$COUNT"
    [ "\$n" = "2" ] && exit 129
  fi
done
exec "\$REALGIT" "\$@"
SHIM
        chmod +x "$G_SHIM_CFGFAIL/git"
        (cd "$GATE_REPO" && PATH="$G_SHIM_CFGFAIL:$PATH" bash "$GATE_IN_CLONE" \
            --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22dr.log" 2>&1)
        G_RC=$?
        if [ ! -f "$G_CFGFAIL_COUNT" ] || [ "$(cat "$G_CFGFAIL_COUNT" 2>/dev/null)" -lt 2 ]; then
            t_fail "identity gate: the setting was never re-read, so 22dr would pass vacuously"
        elif [ "$G_RC" -eq 3 ] \
             && grep -q 'could not read core.useReplaceRefs on at least one side' "$TMP_DIR/gate-22dr.log" \
             && ! grep -q 'core.useReplaceRefs changed' "$TMP_DIR/gate-22dr.log"; then
            t_pass "identity gate: an unreadable setting is unanswerable, not an observed change"
        else
            t_fail "identity gate: a failed setting probe was reported as a change (rc=$G_RC; see $TMP_DIR/gate-22dr.log)"
        fi

        # 22ds: THE CONSUMER IS INSIDE THE DEADLINE TOO. `timeout` wraps the
        # command it is given, so wrapping only the producer left awk outside
        # it: killing git does not finish the command substitution while the
        # consumer holds the pipe (Codex adversarial, section 52 round 19,
        # [medium]).
        G_AWK_SHIM="$TMP_DIR/awkshim-stall-s52"
        mkdir -p "$G_AWK_SHIM"
        cat > "$G_AWK_SHIM/awk" <<'AWKSHIM'
#!/usr/bin/env bash
exec sleep 20
AWKSHIM
        chmod +x "$G_AWK_SHIM/awk"
        G_AWK_T0=$SECONDS
        (cd "$GATE_REPO" && IDENTITY_GATE_BUDGET_SECS=2 PATH="$G_AWK_SHIM:$PATH" \
            bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
            >"$TMP_DIR/gate-22ds.log" 2>&1)
        G_RC=$?
        G_AWK_ELAPSED=$(( SECONDS - G_AWK_T0 ))
        if [ "$G_RC" -ne 0 ] && [ "$G_AWK_ELAPSED" -le 8 ]; then
            t_pass "identity gate: a stalled walk consumer is inside the shared budget (${G_AWK_ELAPSED}s)"
        else
            t_fail "identity gate: the walk consumer escaped the shared budget (rc=$G_RC, ${G_AWK_ELAPSED}s; see $TMP_DIR/gate-22ds.log)"
        fi
        (cd "$GATE_REPO" && git replace -d "$G_S52_ANC" >/dev/null 2>&1)
        (cd "$GATE_REPO" && git replace -d "$G_S52_ANC" >/dev/null 2>&1)
    fi

    # ------------------------------------------------------------------
    # 22ea/22eb/22ec (section 53): WHAT THE PROTOCOL PROBE CAN AND CANNOT
    # VOUCH FOR. `proto_of` must IMPORT the tree under test to read its
    # constants, so tree-controlled code runs inside the very process that
    # produces the gate's record. Section 50 closed every ACCIDENTAL route
    # into that record (import prints to stderr, a multi-line record
    # refused, the record moved to a shell-owned descriptor, the path taken
    # out of argv) and its nineteenth round showed the class is not
    # closable that way. These cases pin what actually holds, in BOTH
    # directions, so the containment is a measured property and not an
    # argument -- and 22eb exists specifically because the argument as
    # first written was wrong.
    # ------------------------------------------------------------------
    if [ -n "${G_PROTO_BASE:-}" ]; then
        g_s53_restore() {
            (cd "$GATE_REPO" || exit 1
             git checkout --quiet "$G_PROTO_BASE" -- \
                 scripts/todo-graph/snapshot_protocol.json \
                 scripts/todo-graph/snapshot_protocol.py \
                 scripts/todo-graph/ref_resolution.py >/dev/null 2>&1
             git commit --quiet --no-verify -am "s53: restore an honest tree" >/dev/null 2>&1) || true
        }
        # EVERY EXIT PATH REAPS, not only the one that reached its assertion.
        # The cancellation cases launch a 600s gate and a 900s adversary; a
        # readiness timeout or a failed identity check is exactly the
        # regression these cases exist to diagnose, and the first cut left
        # both processes running on precisely those branches while deleting
        # the directory they were writing into (Codex adversarial, section 53
        # round 2). Called unconditionally after each case, success or not.
        g_s53_reap() {  # $1.. = pids, group-first then individually
            # SOME OF THESE PIDS COME FROM FILES THE FIXTURE'S OWN ADVERSARY
            # WROTE, so they are not trusted numbers: a sabotaged loader could
            # name this harness, and a recorded process that has already exited
            # could have had its pid reissued. Anything sharing the harness's
            # pid, group or session is refused outright, which is the part that
            # matters -- killing the suite from its own cleanup path would be a
            # far worse failure than a missed reap (Codex adversarial, section
            # 53 round 3, [high]).
            local _p _pg _sid _self_pg _self_sid
            _self_pg="$(ps -o pgid= -p $$ 2>/dev/null | tr -d ' ')"
            _self_sid="$(ps -o sid= -p $$ 2>/dev/null | tr -d ' ')"
            for _p in "$@"; do
                [ -n "$_p" ] || continue
                case "$_p" in ''|*[!0-9]*) continue ;; 0|1) continue ;; esac
                [ "$_p" = "$$" ] && continue
                _pg="$(ps -o pgid= -p "$_p" 2>/dev/null | tr -d ' ')"
                _sid="$(ps -o sid= -p "$_p" 2>/dev/null | tr -d ' ')"
                [ -z "$_pg" ] && continue
                [ -n "$_self_pg" ] && [ "$_pg" = "$_self_pg" ] && continue
                [ -n "$_self_sid" ] && [ "$_sid" = "$_self_sid" ] && continue
                [ "$_pg" = "$_p" ] && kill -KILL -- "-$_pg" 2>/dev/null
                kill -KILL "$_p" 2>/dev/null || true
            done
            return 0
        }
        g_s53_bump_schema() {
            (cd "$GATE_REPO" || exit 1
             python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
assert "snapshot_schema" in d, "protocol file shape changed"
d["snapshot_schema"] = int(d["snapshot_schema"]) + 1
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
            ) || return 1
        }

        # 22ea-control: THE HONEST PATH THIS CASE IS MEASURED AGAINST. The
        # same protocol data change, with the loader untouched, is the
        # data-only migration the gate is designed to pass. Without this the
        # refusal below could be caused by the schema bump alone.
        g_s53_restore
        g_s53_bump_schema
        (cd "$GATE_REPO" && git commit --quiet --no-verify -am "22ea-control: schema bump alone" >/dev/null 2>&1)
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
            >"$TMP_DIR/gate-22ea-control.log" 2>&1)
        G_RC=$?
        if [ "$G_RC" -eq 0 ] && grep -q 'DATA-ONLY' "$TMP_DIR/gate-22ea-control.log"; then
            t_pass "identity gate: CONTROL -- a schema bump with an untouched loader is the data-only PASS"
        else
            t_fail "identity gate: the honest data-only control did not pass, so 22ea proves nothing (rc=$G_RC; see $TMP_DIR/gate-22ea-control.log)"
        fi

        # 22ea: A LOADER EDIT THAT ADMITS THE CHANGE IS REFUSED BY THE
        # EXECUTABLE-CLOSURE CHECK, and named as unseparable rather than
        # adjudicated. The substitution attempt here is the shape section 50
        # already made inert -- a handler registered at import can only
        # APPEND to the record, and the caller takes the FIRST line -- so the
        # record still reports the protocol change, the gate enters the
        # protocol-changed branch, and snapshot_protocol.py being in
        # EXEC_CLOSURE is what refuses the pair.
        g_s53_restore
        g_s53_bump_schema
        (
            cd "$GATE_REPO" || exit 1
            cat >> scripts/todo-graph/snapshot_protocol.py <<'PYSAB'


# 22ea: an import-time attempt to substitute the gate's own record, registered
# to run at interpreter shutdown. It can only APPEND, so it is inert -- which
# is the point of this fixture: the record still tells the truth, and the pair
# is refused for the loader edit rather than adjudicated.
import atexit as _s53_atexit
import os as _s53_os


def _s53_substitute():
    try:
        _s53_os.write(3, b"1|dead|beef|data|e30=|e30=|e30=\n")
    except OSError:
        pass


_s53_atexit.register(_s53_substitute)
PYSAB
            git commit --quiet --no-verify -am "22ea: loader substitution attempt + protocol change" >/dev/null 2>&1
        )
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
            >"$TMP_DIR/gate-22ea.log" 2>&1)
        G_RC=$?
        if [ "$G_RC" -eq 3 ] \
           && grep -q 'snapshot_protocol.py' "$TMP_DIR/gate-22ea.log" \
           && grep -q 'Split the commit' "$TMP_DIR/gate-22ea.log" \
           && ! grep -q 'DATA-ONLY' "$TMP_DIR/gate-22ea.log"; then
            t_pass "identity gate: a loader edit that admits the protocol change is refused, not adjudicated"
        else
            t_fail "identity gate: a sabotaging loader paired with a protocol change was not refused (rc=$G_RC; see $TMP_DIR/gate-22ea.log)"
        fi

        # 22eb: AND THE OTHER HALF OF THE DICHOTOMY, which the section's first
        # draft got wrong. The executable-closure refusal above runs only
        # INSIDE the protocol-changed branch, so a loader that CONCEALS the
        # change -- writing a forged first line to the descriptor before the
        # honest record is printed -- never reaches it. What it forfeits is the
        # fast exit: snapshot_protocol.py is still in CLOSURE, so the gate
        # cannot take the byte-identical early return either, and the pair goes
        # to the FULL resolver differential instead. That is the accurate
        # containment statement, and it is strictly weaker than "refused
        # outright": the honest limit is that a gate which must execute the
        # tree cannot distrust that execution's output.
        g_s53_restore
        G_S53_REC="$( (cd "$GATE_REPO" && python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import base64, hashlib, json, pathlib, sys
d = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
pre, post = list(d["pre_resolution_buckets"]), list(d["post_resolution_buckets"])
def fp(seq):
    return hashlib.sha256(json.dumps(list(seq)).encode("utf-8")).hexdigest()
def b64(o):
    return base64.b64encode(json.dumps(o).encode("utf-8")).decode("ascii")
print("%d|%s|%s|%s|%s|%s|%s" % (
    int(d["snapshot_schema"]), fp(pre), fp(post), "data",
    b64(pre), b64(post), b64([d.get("renamed_buckets") or {}, d.get("retired_buckets") or []])))
PY2
        ) )"
        if [ -z "$G_S53_REC" ]; then
            t_fail "identity gate: could not compute the base protocol record for the concealment case"
        else
            g_s53_bump_schema
            (
                cd "$GATE_REPO" || exit 1
                {
                    printf '\n\n'
                    printf '# 22eb: forge the gate record at IMPORT time, before the honest one is\n'
                    printf '# printed. The caller takes the FIRST line, so this one wins and the head\n'
                    printf '# protocol reads identical to the base despite the data change.\n'
                    printf 'import os as _s53_os2\n\n'
                    printf 'try:\n'
                    printf "    _s53_os2.write(3, b'%s\\\\n')\n" "$G_S53_REC"
                    printf 'except OSError:\n'
                    printf '    pass\n'
                } >> scripts/todo-graph/snapshot_protocol.py
                git commit --quiet --no-verify -am "22eb: loader conceals the protocol change" >/dev/null 2>&1
            )
            (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
                >"$TMP_DIR/gate-22eb.log" 2>&1)
            G_RC=$?
            # PIN THE WHOLE PATH, not just the absence of a fast exit. The
            # concealment hides the change from the PROTOCOL check and then
            # walks straight into it again at compare time, because the two
            # snapshots are written under different schemas -- so a forged
            # record converts a clean data-only PASS into an infrastructure
            # refusal that says in words it is not one.
            if [ "$G_RC" -eq 3 ] \
               && ! grep -q 'DATA-ONLY' "$TMP_DIR/gate-22eb.log" \
               && ! grep -q 'nothing to differentiate' "$TMP_DIR/gate-22eb.log" \
               && grep -q 'walking with BASE and HEAD resolver code' "$TMP_DIR/gate-22eb.log" \
               && grep -q 'baseline schema is' "$TMP_DIR/gate-22eb.log" \
               && grep -q 'this is NOT a pass' "$TMP_DIR/gate-22eb.log"; then
                t_pass "identity gate: a forged record buys no fast exit -- the walk runs and the schema refuses it anyway"
            else
                t_fail "identity gate: a concealed protocol change took a fast path (rc=$G_RC; see $TMP_DIR/gate-22eb.log)"
            fi
        fi

        # 22eb2: AND THE HONEST LIMIT, stated as a test rather than as a
        # sentence. Conceal a VOCABULARY change instead of a schema one and
        # nothing downstream refuses it: both snapshots are written at the same
        # schema, so the differential compares them on their merits and an
        # unused bucket really is verdict-neutral. The concealment therefore
        # buys the tree exactly nothing -- it forfeits the cheap protocol path
        # and receives the expensive check that was always the stronger one.
        # That is the shape of the guarantee this probe can offer: a gate that
        # must EXECUTE the tree cannot distrust that execution's output, so it
        # is built so that lying about the output only ever costs the liar the
        # weaker path.
        g_s53_restore
        if [ -n "$G_S53_REC" ]; then
            (
                cd "$GATE_REPO" || exit 1
                python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["post_resolution_buckets"].append("concealed_and_never_used")
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
                {
                    printf '\n\n'
                    printf '# 22eb2: the same forgery, over a vocabulary change instead of a schema one.\n'
                    printf 'import os as _s53_os4\n\n'
                    printf 'try:\n'
                    printf "    _s53_os4.write(3, b'%s\\\\n')\n" "$G_S53_REC"
                    printf 'except OSError:\n'
                    printf '    pass\n'
                } >> scripts/todo-graph/snapshot_protocol.py
                git commit --quiet --no-verify -am "22eb2: conceal a vocabulary change" >/dev/null 2>&1
            )
            (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_PROTO_BASE" --head HEAD \
                >"$TMP_DIR/gate-22eb2.log" 2>&1)
            G_RC=$?
            if [ "$G_RC" -eq 0 ] \
               && ! grep -q 'DATA-ONLY' "$TMP_DIR/gate-22eb2.log" \
               && grep -q 'walking with BASE and HEAD resolver code' "$TMP_DIR/gate-22eb2.log"; then
                t_pass "identity gate: concealing a vocabulary change only forfeits the cheap path -- the differential still adjudicates it"
            else
                t_fail "identity gate: a concealed vocabulary change did not reach the differential (rc=$G_RC; see $TMP_DIR/gate-22eb2.log)"
            fi
        fi

        # 22ec: THE CANCELLATION PATH, which section 50 reasoned about and
        # never fixtured. The probe imports tree-controlled code, so what it
        # spawns is whatever that tree spawns -- and the old shape
        # (`timeout` with `--foreground`, no recorded pid) left exactly that
        # unbounded: a loader that starts one descendant and stalls kept the
        # descendant alive after the probe was killed at its budget, after
        # `cleanup` finished, and after the gate exited. The probe itself was
        # always bounded; the descendant never was. This case is the proof,
        # and it is deliberately ADVERSARIAL: the stall ignores TERM, so a
        # cooperative `sleep` cannot pass it by accident.
        g_s53_restore
        G_S53_RUN="$TMP_DIR/s53-cancel"
        rm -rf "$G_S53_RUN"; mkdir -p "$G_S53_RUN/tmp"
        (
            cd "$GATE_REPO" || exit 1
            {
                printf '\n\n'
                printf '# 22ec: an adversarial stall inside the imported tree code.\n'
                printf 'import os as _s53_os3, signal as _s53_sig, subprocess as _s53_sp, time as _s53_t\n\n'
                printf '_s53_sig.signal(_s53_sig.SIGTERM, _s53_sig.SIG_IGN)\n'
                printf '_s53_sig.signal(_s53_sig.SIGINT, _s53_sig.SIG_IGN)\n'
                printf '_s53_kid = _s53_sp.Popen(["sleep", "900"])\n'
                printf "open('%s/desc.pid', 'w').write(str(_s53_kid.pid))\n" "$G_S53_RUN"
                printf "open('%s/probe.pid', 'w').write(str(_s53_os3.getpid()))\n" "$G_S53_RUN"
                printf "open('%s/ready', 'w').write('1')\n" "$G_S53_RUN"
                printf '_s53_t.sleep(900)\n'
            } >> scripts/todo-graph/snapshot_protocol.py
            git commit --quiet --no-verify -am "22ec: a stalling loader" >/dev/null 2>&1
        )
        G_S53_WT_BEFORE="$( (cd "$GATE_REPO" && git worktree list --porcelain 2>/dev/null) )"
        # OWN THE TEMP ROOT so "no tempdir left behind" is checkable: the gate
        # mktemps under TMPDIR, and an empty fixture-owned directory afterwards
        # is the whole assertion.
        # KEEP THE PID. Finding the gate with `pgrep -f` later would signal
        # whatever matched first -- a concurrent fixture, a stale gate, or an
        # operator's own run -- and the process group it kills is then not
        # provably the one under test (Codex adversarial, section 53, [high]).
        # `setsid` execs in place here (a background command in a job-control-
        # less shell is not a group leader), so `$!` IS the new session leader,
        # which the identity check below requires rather than assumes.
        TMPDIR="$G_S53_RUN/tmp" setsid bash -c \
            'cd "$1" && exec bash "$2" --base "$3" --head HEAD' _ \
            "$GATE_REPO" "$GATE_IN_CLONE" "$G_PROTO_BASE" \
            >"$TMP_DIR/gate-22ec.log" 2>&1 &
        G_S53_GATE=$!
        # READINESS, NOT A SLEEP. The gate materializes a base worktree and
        # probes the base tree before it ever reaches the head probe, and that
        # is the part whose duration a loaded host changes.
        G_S53_READY=0
        for _i in $(seq 1 600); do
            [ -f "$G_S53_RUN/ready" ] && { G_S53_READY=1; break; }
            sleep 0.2
        done
        if [ "$G_S53_READY" -ne 1 ]; then
            t_fail "identity gate: the stalling probe never became ready, so the cancellation case ran nothing (see $TMP_DIR/gate-22ec.log)"
            g_s53_reap "$G_S53_GATE" "$(cat "$G_S53_RUN/probe.pid" 2>/dev/null)" "$(cat "$G_S53_RUN/desc.pid" 2>/dev/null)"
        else
            G_S53_PROBE="$(cat "$G_S53_RUN/probe.pid" 2>/dev/null)"
            G_S53_DESC="$(cat "$G_S53_RUN/desc.pid" 2>/dev/null)"
            G_S53_PROBEPG="$(ps -o pgid= -p "$G_S53_PROBE" 2>/dev/null | tr -d ' ')"
            # SIGNAL THE GATE'S GROUP, which is what an external escalation
            # does -- but PROVE it is the gate's group first. The launched pid
            # must be its own process-group AND session leader, and that group
            # must not be this harness's, or the signal below would take the
            # suite down with it.
            G_S53_GATEPG="$(ps -o pgid= -p "$G_S53_GATE" 2>/dev/null | tr -d ' ')"
            G_S53_GATESID="$(ps -o sid= -p "$G_S53_GATE" 2>/dev/null | tr -d ' ')"
            G_S53_SELFPG="$(ps -o pgid= -p $$ 2>/dev/null | tr -d ' ')"
            if [ -z "$G_S53_GATEPG" ] || [ "$G_S53_GATEPG" != "$G_S53_GATE" ] \
               || [ "$G_S53_GATESID" != "$G_S53_GATE" ] \
               || [ "$G_S53_GATEPG" = "$G_S53_SELFPG" ]; then
                t_fail "identity gate: the launched gate is not an isolated session leader (pid=$G_S53_GATE pgid=$G_S53_GATEPG sid=$G_S53_GATESID harness=$G_S53_SELFPG), so the cancellation case will not signal it"
                g_s53_reap "$G_S53_GATE" "$G_S53_PROBE" "$G_S53_DESC"
            else
                G_S53_T0=$SECONDS
                kill -TERM -- "-$G_S53_GATEPG" 2>/dev/null || true
                # POST-SIGNAL DEADLINE, kept well under the 60s probe cap so a
                # prompt exit cannot be confused with the budget expiring.
                for _i in $(seq 1 100); do
                    kill -0 "$G_S53_GATE" 2>/dev/null || break
                    sleep 0.2
                done
                G_S53_ELAPSED=$(( SECONDS - G_S53_T0 ))
                # Give the reap loop its escalation window before judging.
                for _i in $(seq 1 60); do
                    kill -0 "$G_S53_DESC" 2>/dev/null || break
                    sleep 0.2
                done
                G_S53_GATE_LIVE=no; kill -0 "$G_S53_GATE" 2>/dev/null && G_S53_GATE_LIVE=yes
                G_S53_PROBE_LIVE=no; kill -0 "$G_S53_PROBE" 2>/dev/null && G_S53_PROBE_LIVE=yes
                G_S53_DESC_LIVE=no; kill -0 "$G_S53_DESC" 2>/dev/null && G_S53_DESC_LIVE=yes
                G_S53_WT_AFTER="$( (cd "$GATE_REPO" && git worktree list --porcelain 2>/dev/null) )"
                G_S53_TMP_LEFT="$(ls -A "$G_S53_RUN/tmp" 2>/dev/null)"
                if [ "$G_S53_GATE_LIVE" = no ] && [ "$G_S53_ELAPSED" -le 20 ] \
                   && [ "$G_S53_PROBE_LIVE" = no ] && [ "$G_S53_DESC_LIVE" = no ] \
                   && [ "$G_S53_WT_BEFORE" = "$G_S53_WT_AFTER" ] \
                   && [ -z "$G_S53_TMP_LEFT" ]; then
                    t_pass "identity gate: cancelling the gate reaps the probe AND what the tree spawned (${G_S53_ELAPSED}s)"
                else
                    t_fail "identity gate: cancellation left something behind (${G_S53_ELAPSED}s gate=$G_S53_GATE_LIVE probe=$G_S53_PROBE_LIVE desc=$G_S53_DESC_LIVE tmp='$G_S53_TMP_LEFT' worktrees_equal=$([ "$G_S53_WT_BEFORE" = "$G_S53_WT_AFTER" ] && echo yes || echo no); see $TMP_DIR/gate-22ec.log)"
                fi
                # The fixture never leaves its own adversary running, whatever
                # the verdict was.
                [ -n "$G_S53_PROBEPG" ] && kill -KILL -- "-$G_S53_PROBEPG" 2>/dev/null
                g_s53_reap "$G_S53_DESC" "$G_S53_PROBE" "$G_S53_GATE"

                # MUTATION: stop the probe being tracked in `WALK_PIDS`, and
                # the SAME adversary must survive. Without this the case
                # cannot tell a reaped probe from one that never started, and
                # that one assignment is the whole of what lets `cleanup`
                # reach the probe's session at all.
                G_MUT_S53="$GATE_REPO/scripts/todo-graph/identity-gate-noreap.sh"
                sed 's#^    WALK_PIDS="$_prb"$#    WALK_PIDS=""#' \
                    "$GATE_IN_CLONE" > "$G_MUT_S53"
                if grep -q '^    WALK_PIDS="\$_prb"$' "$G_MUT_S53"; then
                    t_fail "identity gate: could not build the probe-reap mutation (the shape moved)"
                else
                    rm -f "$G_S53_RUN/ready" "$G_S53_RUN/probe.pid" "$G_S53_RUN/desc.pid"
                    TMPDIR="$G_S53_RUN/tmp" setsid bash -c \
                        'cd "$1" && exec bash "$2" --base "$3" --head HEAD' _ \
                        "$GATE_REPO" "$G_MUT_S53" "$G_PROTO_BASE" \
                        >"$TMP_DIR/gate-22ec-mut.log" 2>&1 &
                    G_S53_MGATE=$!
                    G_S53_MREADY=0
                    for _i in $(seq 1 600); do
                        [ -f "$G_S53_RUN/ready" ] && { G_S53_MREADY=1; break; }
                        sleep 0.2
                    done
                    if [ "$G_S53_MREADY" -ne 1 ]; then
                        t_fail "identity gate: the mutation's probe never became ready (see $TMP_DIR/gate-22ec-mut.log)"
                        g_s53_reap "$G_S53_MGATE" "$(cat "$G_S53_RUN/probe.pid" 2>/dev/null)" "$(cat "$G_S53_RUN/desc.pid" 2>/dev/null)"
                    else
                        G_S53_MPROBE="$(cat "$G_S53_RUN/probe.pid" 2>/dev/null)"
                        G_S53_MDESC="$(cat "$G_S53_RUN/desc.pid" 2>/dev/null)"
                        G_S53_MPPG="$(ps -o pgid= -p "$G_S53_MPROBE" 2>/dev/null | tr -d ' ')"
                        G_S53_MGPG="$(ps -o pgid= -p "$G_S53_MGATE" 2>/dev/null | tr -d ' ')"
                        G_S53_MSID="$(ps -o sid= -p "$G_S53_MGATE" 2>/dev/null | tr -d ' ')"
                        # THE MUTATION MUST FAIL FOR THE RIGHT REASON. Without
                        # these preconditions a lookup miss, an undelivered
                        # signal or a gate that never exited would leave the
                        # descendant alive and the arm would read as a leak it
                        # never demonstrated (Codex test-coverage, section 53).
                        G_S53_MSIG=0
                        if [ -n "$G_S53_MGPG" ] && [ "$G_S53_MGPG" = "$G_S53_MGATE" ] \
                           && [ "$G_S53_MSID" = "$G_S53_MGATE" ]; then
                            kill -TERM -- "-$G_S53_MGPG" 2>/dev/null && G_S53_MSIG=1
                        fi
                        for _i in $(seq 1 100); do
                            kill -0 "$G_S53_MGATE" 2>/dev/null || break
                            sleep 0.2
                        done
                        G_S53_MGATE_LIVE=no; kill -0 "$G_S53_MGATE" 2>/dev/null && G_S53_MGATE_LIVE=yes
                        for _i in $(seq 1 30); do
                            kill -0 "$G_S53_MDESC" 2>/dev/null || break
                            sleep 0.2
                        done
                        if [ "$G_S53_MSIG" -ne 1 ] || [ "$G_S53_MGATE_LIVE" != no ]; then
                            t_fail "identity gate: the mutation never delivered cancellation (signalled=$G_S53_MSIG gate_live=$G_S53_MGATE_LIVE pid=$G_S53_MGATE pgid=$G_S53_MGPG sid=$G_S53_MSID), so it cannot show a leak"
                        elif kill -0 "$G_S53_MDESC" 2>/dev/null; then
                            t_pass "identity gate: MUTATION -- an unpublished probe pid leaks the tree's descendant, so 22ec measures the reaping"
                        else
                            t_fail "identity gate: the probe-reap mutation did not leak, so 22ec proves nothing (see $TMP_DIR/gate-22ec-mut.log)"
                        fi
                        [ -n "$G_S53_MPPG" ] && kill -KILL -- "-$G_S53_MPPG" 2>/dev/null
                        g_s53_reap "$G_S53_MDESC" "$G_S53_MPROBE" "$G_S53_MGATE"
                    fi
                    rm -f "$G_MUT_S53"
                fi
            fi
        fi
        rm -rf "$G_S53_RUN"

        # 22ed: THE ORDERING 22ec CANNOT REACH. 22ec's loader STALLS, so the
        # pid file is still published when the signal arrives and `cleanup`
        # does the reaping. A loader that spawns a descendant and then RETURNS
        # NORMALLY takes the opposite path: `timeout` exits 0, `proto_of`
        # unpublishes the pid, `cleanup` finds nothing, and the gate runs to a
        # perfectly ordinary verdict with the descendant still alive. No signal
        # is sent anywhere in this case -- the gate simply finishes -- which is
        # what makes it the honest test of the success path (Codex
        # test-coverage, section 53, [high]).
        g_s53_restore
        G_S53_RUN2="$TMP_DIR/s53-normal"
        rm -rf "$G_S53_RUN2"; mkdir -p "$G_S53_RUN2/tmp"
        (
            cd "$GATE_REPO" || exit 1
            {
                printf '\n\n'
                printf '# 22ed: spawn a descendant and return normally, IN THE PROBE ONLY.\n'
                printf '# This same loader is imported by every python phase the gate runs -- the\n'
                printf '# two resolver walks, the comparison and the cache build all reach it -- so\n'
                printf '# an unconditional spawn would record whichever phase happened to run last\n'
                printf '# and the case would not be about the probe at all. fd 3 is open only in\n'
                printf '# the probe, because the SHELL opens it there to carry the record.\n'
                printf 'import os as _s53_os5, subprocess as _s53_sp2\n\n'
                printf 'try:\n'
                printf '    _s53_os5.fstat(3)\n'
                printf '    _s53_in_probe = True\n'
                printf 'except OSError:\n'
                printf '    _s53_in_probe = False\n\n'
                printf 'if _s53_in_probe:\n'
                printf '    _s53_kid2 = _s53_sp2.Popen(["sleep", "900"])\n'
                printf "    open('%s/desc.pid', 'w').write(str(_s53_kid2.pid))\n" "$G_S53_RUN2"
            } >> scripts/todo-graph/snapshot_protocol.py
            git commit --quiet --no-verify -am "22ed: a loader that spawns and returns" >/dev/null 2>&1
        )
        (cd "$GATE_REPO" && TMPDIR="$G_S53_RUN2/tmp" bash "$GATE_IN_CLONE" \
            --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22ed.log" 2>&1)
        G_RC=$?
        G_S53_ND="$(cat "$G_S53_RUN2/desc.pid" 2>/dev/null)"
        if [ -z "$G_S53_ND" ]; then
            t_fail "identity gate: the normal-return loader never spawned, so 22ed ran nothing (rc=$G_RC; see $TMP_DIR/gate-22ed.log)"
        else
            G_S53_ND_LIVE=no; kill -0 "$G_S53_ND" 2>/dev/null && G_S53_ND_LIVE=yes
            G_S53_TMP2="$(ls -A "$G_S53_RUN2/tmp" 2>/dev/null)"
            if [ "$G_S53_ND_LIVE" = no ] && [ -z "$G_S53_TMP2" ]; then
                t_pass "identity gate: a loader that spawns and RETURNS still leaves nothing behind"
            else
                t_fail "identity gate: the success path leaked what the tree spawned (desc=$G_S53_ND_LIVE tmp='$G_S53_TMP2'; see $TMP_DIR/gate-22ed.log)"
            fi
            g_s53_reap "$G_S53_ND"
        fi

        # MUTATION: take the post-`wait` group reap back out, and the SAME
        # loader must leak. This is the whole content of 22ed -- without it the
        # case cannot tell a reaped descendant from one that was never started.
        G_MUT_S53B="$GATE_REPO/scripts/todo-graph/identity-gate-noreap2.sh"
        # RE-ANCHORED ON THE EXTRACTED HELPER (section 56). The reap this case
        # mutates used to be written out inline at every site; it is now one
        # `reap_walk_group` call per phase, so the anchor is that CALL. Removing
        # the call at the probe leaves every other phase reaping, which is what
        # keeps this case about the probe.
        # THE ANCHOR'S ABSENCE ALONE IS NOT PROOF. If the source anchor MOVES,
        # sed matches nothing, the mutant is an unchanged copy -- and the
        # anchor is absent from it for the wrong reason, so this guard used to
        # wave the case through against a non-mutant (Codex consistency,
        # section 56, [medium]). `g_mutant_ok` requires a non-empty, parseable
        # file that DIFFERS from its source, which is what 22ew and 22ey
        # already do; the absence check then says which line changed.
        sed 's#^    reap_walk_group "$_prb"$#    :#' \
            "$GATE_IN_CLONE" > "$G_MUT_S53B"
        if ! g_mutant_ok "$GATE_IN_CLONE" "$G_MUT_S53B" 'reap_walk_group' \
           || grep -q '^    reap_walk_group "\$_prb"$' "$G_MUT_S53B"; then
            t_fail "identity gate: could not build the success-path reap mutation (the shape moved)"
        else
            rm -f "$G_S53_RUN2/desc.pid"
            (cd "$GATE_REPO" && TMPDIR="$G_S53_RUN2/tmp" bash "$G_MUT_S53B" \
                --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22ed-mut.log" 2>&1)
            G_S53_MND="$(cat "$G_S53_RUN2/desc.pid" 2>/dev/null)"
            if [ -n "$G_S53_MND" ] && kill -0 "$G_S53_MND" 2>/dev/null; then
                t_pass "identity gate: MUTATION -- without the success-path reap the descendant survives, so 22ed measures it"
            else
                t_fail "identity gate: the success-path reap mutation did not leak, so 22ed proves nothing (see $TMP_DIR/gate-22ed-mut.log)"
            fi
            g_s53_reap "$G_S53_MND"
            rm -f "$G_MUT_S53B"
        fi
        rm -rf "$G_S53_RUN2"
        g_s53_restore

        # ------------------------------------------------------------------
        # 22eu: THE SUPERVISED PHASES 22ed CANNOT REACH (section 56).
        #
        # 22ed proves the ordering for the protocol PROBE. Every other
        # supervised phase in the gate has the identical shape -- record the
        # leader in WALK_PIDS, `wait`, clear WALK_PIDS -- and until section 56
        # none of them signalled on that path, so `cleanup` (the only other
        # thing that ever signals) had nothing left to find. Measured before
        # the fix: the gate exited 0 with its ordinary PASS verdict and the
        # descendants of the two resolver walks were still alive.
        #
        # ONE POSITIVE RUN, THEN ONE MUTATION PER PHASE THAT ACTUALLY FIRED.
        # The mutation removes the reap at ONE phase and requires THAT phase's
        # descendant to survive while the others still die, which is what
        # proves per-site wiring rather than merely that the shared helper
        # works. Phases that did not run in this configuration produce no pid
        # file and are not mutated, so the case scopes itself instead of
        # asserting about a phase the gate skipped.
        g_s56_restore() {
            (cd "$GATE_REPO" || exit 1
             git checkout --quiet "$G_PROTO_BASE" -- \
                 scripts/todo-graph/cache_schema.py \
                 scripts/todo-graph/producer_differential.py \
                 scripts/todo-graph/build.py >/dev/null 2>&1
             git commit --quiet --no-verify -am "s56: restore an honest tree" >/dev/null 2>&1) || true
        }
        g_s56_loader() {   # $1 = run dir, $2 = spawn kwargs, $3 = pid-file suffix
            printf '\n\n'
            printf '# section 56: spawn a descendant and return normally, ONE PER PHASE.\n'
            printf '# Keyed on argv, because these modules are imported by several phases and\n'
            printf '# an ungated spawn would record whichever ran last -- the scoping mistake\n'
            printf '# section 53 paid for once already.\n'
            printf 'import os as _s56_os, sys as _s56_sys, subprocess as _s56_sp\n'
            printf 'def _s56_gate_own(_p):\n'
            printf '    # THE GATE OWN TEMPORARY DIRECTORY, not merely a matching\n'
            printf '    # basename. The producer differential runs `build.py` ITSELF, with\n'
            printf '    # its own `--output` under its own tempdir, so keying on the\n'
            printf '    # basename alone attributed one of ITS builds to the gate head cache\n'
            printf '    # phase and the case stopped being about the phase it names -- the\n'
            printf '    # same scoping mistake section 53 paid for, in a new shape.\n'
            printf '    if _p.startswith("/proc/self/fd/"):\n'
            printf '        # section 60: the gate reaches its own cache/walk/comparison\n'
            printf '        # paths through its inherited TMP_FD descriptor rather than by\n'
            printf '        # name, so an fd-relative path IS the gate-own signal now -- only\n'
            printf '        # a process holding that inherited fd can construct one.\n'
            printf '        return True\n'
            printf '    return _s56_os.path.basename(_s56_os.path.dirname(_p)).startswith("identity-gate.")\n'
            printf 'def _s56_phase():\n'
            printf '    _a = [str(_x) for _x in _s56_sys.argv]\n'
            printf '    _p0 = _s56_os.path.basename(_a[0]) if _a else ""\n'
            printf '    if _p0 == "producer_differential.py":\n'
            printf '        return "prod"\n'
            printf '    if _p0 == "build.py":\n'
            printf '        for _i, _x in enumerate(_a):\n'
            printf '            if _x == "--output" and _i + 1 < len(_a):\n'
            printf '                if not _s56_gate_own(_a[_i+1]):\n'
            printf '                    return ""\n'
            printf '                return "bcache" if _s56_os.path.basename(_a[_i+1]) == "todo-cache-base.json" else "hcache"\n'
            printf '        return ""\n'
            printf '    if _p0 == "corpus_resolution_snapshot.py":\n'
            printf '        if len(_a) > 2 and _a[1] == "compare" and _s56_gate_own(_a[2]):\n'
            printf '            return "cmp"\n'
            printf '        if len(_a) > 2 and _a[1] == "write" and _s56_gate_own(_a[2]):\n'
            printf '            return "hwalk" if _s56_os.path.basename(_a[2]).startswith("head") else "bwalk"\n'
            printf '    return ""\n'
            printf '_s56_p = _s56_phase()\n'
            printf 'if _s56_p:\n'
            printf '    # EVERY STREAM TO DEVNULL. The descendant would otherwise INHERIT its\n'
            printf '    # phase stdio, and the producer differential runs `build.py` under\n'
            printf '    # `capture_output=True` -- a pipe whose read does not finish until\n'
            printf '    # every writer closes it. A `sleep 900` holding that pipe hung the\n'
            printf '    # differential until the budget killed it, so the fixture measured a\n'
            printf '    # timeout instead of a reap (measured, section 56).\n'
            printf '    _s56_kid = _s56_sp.Popen(["sleep", "900"], stdin=_s56_sp.DEVNULL, stdout=_s56_sp.DEVNULL, stderr=_s56_sp.DEVNULL%s)\n' "$2"
            printf "    open('%s/desc' + _s56_p + '%s', 'w').write(str(_s56_kid.pid))\n" "$1" "$3"
        }
        g_s56_inject() {   # $1 = loader file; injects into both import roots
            # `cache_schema` is APPENDED to: it is a plain module, so
            # module-level code at the end runs on import, and it is what the
            # two cache builds, the two resolver walks and the comparison all
            # reach.
            #
            # `producer_differential` is SPLICED IN AT THE TOP, and that
            # difference is load-bearing rather than stylistic. The file ends
            # with `sys.exit(main(sys.argv[1:]))`, so anything appended after it
            # is never reached -- measured: an appended loader left the `prod`
            # phase silently uncovered while every other phase reported
            # correctly, which is precisely the shape the coverage check below
            # exists to catch.
            (cd "$GATE_REPO" || exit 1
             cat "$1" >> scripts/todo-graph/cache_schema.py
             python3 - scripts/todo-graph/producer_differential.py "$1" <<'S56PD'
import pathlib, sys
p = pathlib.Path(sys.argv[1]); t = p.read_text(encoding="utf-8")
key = "from __future__ import annotations\n"
if key not in t:
    raise SystemExit("producer_differential.py __future__ import moved")
p.write_text(t.replace(key, key + pathlib.Path(sys.argv[2]).read_text(encoding="utf-8"), 1),
             encoding="utf-8")
S56PD
            ) || return 1
            return 0
        }
        g_s56_gone() {   # $1 = pid; 0 once it is really gone, bounded
            # POLLED, NOT PROBED ONCE. `kill -KILL` is immediate but REAPING is
            # not: these descendants are reparented when their leader exits, so
            # `kill -0` still succeeds on the zombie for as long as it takes
            # init to collect it. A single check therefore fails intermittently
            # -- observed in the suite while the identical arm passed standalone
            # (section 56).
            local _i _n
            _n="${2:-60}"
            for _i in $(seq 1 "$_n"); do
                kill -0 "$1" 2>/dev/null || return 0
                sleep 0.1
            done
            return 1
        }
        g_s56_collect() {   # $1 = run dir; ANSWERS in G_S56_RAN / G_S56_LIVE
            local _pf _ph _pid
            G_S56_RAN=""; G_S56_LIVE=""
            for _pf in "$1"/desc*.pid; do
                [ -e "$_pf" ] || continue
                _ph="${_pf##*/desc}"; _ph="${_ph%.pid}"
                _pid="$(cat "$_pf" 2>/dev/null)"
                case "$_pid" in ''|*[!0-9]*) continue ;; esac
                G_S56_RAN="$G_S56_RAN $_ph"
                # BOUNDED-POLLED before believing "still alive", for the same
                # reaping race `g_s56_gone` exists for: a just-KILLed descendant
                # answers `kill -0` until init collects it. A FIXED settle was
                # the first cut and is the shape that flakes under load (Codex
                # perf, section 56, [medium]). Two seconds, not six: the arms
                # that legitimately expect a LIVE descendant pay this bound in
                # full, so it is the reaping delay it has to cover, not a
                # generous timeout.
                g_s56_gone "$_pid" 20 || G_S56_LIVE="$G_S56_LIVE $_ph"
                g_s53_reap "$_pid"
            done
        }
        g_s56_pidvar() {   # $1 = phase key -> the gate's leader variable
            case "$1" in
                prod)   printf 'PROD_PID' ;;
                hcache) printf 'CACHE_PID' ;;
                bcache) printf 'BCACHE_PID' ;;
                bwalk)  printf 'BASE_PID' ;;
                hwalk)  printf 'HEAD_PID' ;;
                cmp)    printf 'CMP_PID' ;;
                *)      printf '' ;;
            esac
        }

        g_s56_restore
        G_S56_RUN="$TMP_DIR/s56-normal"
        rm -rf "$G_S56_RUN"; mkdir -p "$G_S56_RUN/tmp"
        G_S56_LOADER="$TMP_DIR/s56-loader.py"
        g_s56_loader "$G_S56_RUN" "" ".pid" > "$G_S56_LOADER"
        # BOTH MODULES, because no single one is imported by every phase:
        # `cache_schema` reaches the two cache builds, the two resolver walks
        # and the comparison, and `producer_differential` imports nothing local
        # at all. Both are already in the gate's EXEC_CLOSURE, which is the same
        # surface 22ed appends to.
        g_s56_inject "$G_S56_LOADER" \
            || t_fail "identity gate: could not inject the 22eu loader (producer_differential.py moved)"
        (cd "$GATE_REPO" && git commit --quiet --no-verify -am "22eu: loaders that spawn and return, one per supervised phase" >/dev/null 2>&1)
        (cd "$GATE_REPO" && TMPDIR="$G_S56_RUN/tmp" bash "$GATE_IN_CLONE" \
            --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22eu.log" 2>&1)
        G_S56_RC=$?
        g_s56_collect "$G_S56_RUN"
        G_S56_PHASES="$G_S56_RAN"
        if [ -z "$G_S56_PHASES" ]; then
            t_fail "identity gate: no supervised phase reached the 22eu loader, so the case measured nothing (rc=$G_S56_RC; see $TMP_DIR/gate-22eu.log)"
        elif [ -n "$G_S56_LIVE" ]; then
            t_fail "identity gate: supervised phases leaked what the tree spawned on their SUCCESS path:$G_S56_LIVE (ran:$G_S56_PHASES; see $TMP_DIR/gate-22eu.log)"
        else
            t_pass "identity gate: every supervised phase that ran ($G_S56_PHASES) left nothing behind on its success path"
        fi
        G_S56_COVERED="$G_S56_PHASES"

        # NO MUTATION LOOP HERE. 22ew below mutates every one of the six sites
        # in a configuration that reaches all of them, so repeating three of
        # them in this one would buy nothing and cost three more end-to-end gate
        # invocations in a suite already charged to every qualifying pre-push.
        # What this case owns is that the DEFAULT configuration -- no contract
        # divergence, which is what CI sees on almost every push -- leaks
        # nothing.
        rm -rf "$G_S56_RUN"
        g_s56_restore

        # ------------------------------------------------------------------
        # 22ew: THE THREE PHASES 22eu's CONFIGURATION CANNOT REACH (section 56).
        #
        # 22eu appends its loader at HEAD only, so the BASE-side phases import
        # a module that does not carry it -- and two further phases are
        # CONDITIONAL: the producer differential is skipped while `build.py` is
        # byte-identical base..head with an unmoved contract, and the base-side
        # cache build runs only when the producer contract identity DIVERGED.
        # Measured: 22eu covers cmp, hcache and hwalk and nothing else, which
        # would have left three of the six reap sites asserted about by a case
        # that never executed them. Coverage is therefore CHECKED at the end of
        # this block rather than assumed from the fixture's intent.
        #
        # So this case commits the loader FIRST and takes that commit as the
        # base -- both worktrees then carry it -- and then moves
        # `CACHE_FORMAT_VERSION` at head, which is the documented hand-bump for
        # a semantic change and is what turns both conditional phases on.
        g_s56_restore
        G_S56_RUN2="$TMP_DIR/s56-bothsides"
        rm -rf "$G_S56_RUN2"; mkdir -p "$G_S56_RUN2/tmp"
        G_S56_LOADER2="$TMP_DIR/s56-loader-both.py"
        g_s56_loader "$G_S56_RUN2" "" ".pid" > "$G_S56_LOADER2"
        g_s56_inject "$G_S56_LOADER2" \
            || t_fail "identity gate: could not inject the 22ew loader (producer_differential.py moved)"
        (cd "$GATE_REPO" && git commit --quiet --no-verify -am "22ew: loaders that spawn and return, on BOTH sides" >/dev/null 2>&1)
        G_S56_BOTH_BASE="$(cd "$GATE_REPO" && git rev-parse HEAD 2>/dev/null)"
        G_S56_SETUP_OK=1
        (
            cd "$GATE_REPO" || exit 1
            python3 - scripts/todo-graph/cache_schema.py <<'S56PY'
import pathlib, re, sys
p = pathlib.Path(sys.argv[1]); t = p.read_text(encoding="utf-8")
t2, n = re.subn(r'^CACHE_FORMAT_VERSION = (\d+)$',
                lambda m: "CACHE_FORMAT_VERSION = %d" % (int(m.group(1)) + 1),
                t, count=1, flags=re.M)
if n != 1:
    raise SystemExit("CACHE_FORMAT_VERSION declaration moved")
p.write_text(t2, encoding="utf-8")
S56PY
        ) || G_S56_SETUP_OK=0
        # NO PRODUCER CHANGE. `build.py` is deliberately left alone: a moved
        # contract identity ALONE already runs the differential ("byte-identical
        # but the contract identity moved; running it anyway"), and it is what
        # turns the base-side cache build on. Changing build.py as well made the
        # differential compare two genuinely different producers and the gate
        # stopped before the walks -- measured, and it cost the three phases
        # this case exists to reach.
        (cd "$GATE_REPO" && git commit --quiet --no-verify -am "22ew: diverge the producer contract at head" >/dev/null 2>&1)
        if [ "$G_S56_SETUP_OK" -ne 1 ] || [ -z "$G_S56_BOTH_BASE" ]; then
            t_fail "identity gate: could not build the 22ew both-sides fixture (the CACHE_FORMAT_VERSION declaration moved, or the base commit could not be read)"
        else
            (cd "$GATE_REPO" && TMPDIR="$G_S56_RUN2/tmp" bash "$GATE_IN_CLONE" \
                --base "$G_S56_BOTH_BASE" --head HEAD >"$TMP_DIR/gate-22ew.log" 2>&1)
            G_S56_RC2=$?
            g_s56_collect "$G_S56_RUN2"
            if [ -z "$G_S56_RAN" ]; then
                t_fail "identity gate: no supervised phase reached the 22ew loader, so the base-side and conditional sites were not measured (rc=$G_S56_RC2; see $TMP_DIR/gate-22ew.log)"
            elif [ -n "$G_S56_LIVE" ]; then
                t_fail "identity gate: supervised phases leaked what the tree spawned on their SUCCESS path:$G_S56_LIVE (ran:$G_S56_RAN; see $TMP_DIR/gate-22ew.log)"
            else
                t_pass "identity gate: with the loader on BOTH sides every phase that ran ($G_S56_RAN) still left nothing behind"
            fi
            # EVERY PHASE THIS CASE RAN IS MUTATED, not only the ones 22eu did
            # not reach: the mutation is what turns "the descendant is gone"
            # into "this site reaped it", and a site proved only by a positive
            # case is a site the fixture cannot distinguish from one that never
            # spawned.
            G_S56_COVERED="$G_S56_RAN"
            for G_S56_PH in $G_S56_RAN; do
                G_S56_VAR="$(g_s56_pidvar "$G_S56_PH")"
                if [ -z "$G_S56_VAR" ]; then
                    t_fail "identity gate: 22ew saw an unknown phase key '$G_S56_PH', so its mutation table is stale"
                    continue
                fi
                G_S56_MUT2="$GATE_REPO/scripts/todo-graph/identity-gate-noreap-$G_S56_PH.sh"
                sed "s#^\([ ]*\)reap_walk_group \"\\\$$G_S56_VAR\"\$#\1:#" \
                    "$GATE_IN_CLONE" > "$G_S56_MUT2"
                if ! g_mutant_ok "$GATE_IN_CLONE" "$G_S56_MUT2" 'reap_walk_group' \
                   || grep -q "^[ ]*reap_walk_group \"\$$G_S56_VAR\"\$" "$G_S56_MUT2"; then
                    t_fail "identity gate: could not build the $G_S56_PH success-path reap mutation (the shape moved)"
                    rm -f "$G_S56_MUT2"
                    continue
                fi
                rm -f "$G_S56_RUN2"/desc*.pid
                (cd "$GATE_REPO" && TMPDIR="$G_S56_RUN2/tmp" bash "$G_S56_MUT2" \
                    --base "$G_S56_BOTH_BASE" --head HEAD >"$TMP_DIR/gate-22ew-$G_S56_PH.log" 2>&1)
                g_s56_collect "$G_S56_RUN2"
                case " $G_S56_LIVE " in
                    *" $G_S56_PH "*)
                        t_pass "identity gate: MUTATION -- without its own reap call the $G_S56_PH phase leaks, so 22ew measures that site" ;;
                    *)
                        t_fail "identity gate: removing the $G_S56_PH reap did not leak, so 22ew proves nothing about that site (ran:$G_S56_RAN live:$G_S56_LIVE; see $TMP_DIR/gate-22ew-$G_S56_PH.log)" ;;
                esac
                rm -f "$G_S56_MUT2"
            done
        fi
        rm -rf "$G_S56_RUN2"
        g_s56_restore

        # COVERAGE IS CHECKED, NOT ASSUMED. Every leader this file `wait`s for
        # and then reaps must have been exercised by one of the two cases above;
        # a phase the fixture's configuration never triggers is a site asserted
        # about by a case that did not run it, which is the failure 22eu had on
        # its own. A NEW supervised phase added later without a fixture fails
        # here rather than shipping unmeasured.
        G_S56_MISSING=""
        for G_S56_PH in prod hcache bcache bwalk hwalk cmp; do
            case " $G_S56_COVERED " in
                *" $G_S56_PH "*) ;;
                *) G_S56_MISSING="$G_S56_MISSING $G_S56_PH" ;;
            esac
        done
        if [ -n "$G_S56_MISSING" ]; then
            t_fail "identity gate: supervised phases never mutation-proved by 22ew:$G_S56_MISSING (covered:$G_S56_COVERED) -- their success-path reaps are unmeasured"
        else
            t_pass "identity gate: all six supervised phases reaped on their success path, each mutation-proved at its own site"
        fi

        # ------------------------------------------------------------------
        # 22ev: THE LIMIT, MEASURED RATHER THAN BELIEVED (section 56).
        #
        # A group signal reaches a descendant only while it is IN the group. A
        # child started with `start_new_session=True`, `setsid` or `setpgid`
        # leaves it, so neither the success-path reap nor `cleanup` can touch
        # it. This case pins that as the SHIPPED behaviour so the boundary is a
        # measurement rather than an assumption.
        #
        # AND IT IS NOT A CLAIM THAT NOTHING WOULD CONTAIN IT. Measured on the
        # development host with a control that had to fire (util-linux 2.39.3,
        # WSL2 kernel 6.18.33.2): a detached child SURVIVED with no namespace
        # and was KILLED when its phase ran as the init of an unprivileged
        # `unshare --user --map-root-user --pid --fork` namespace, because the
        # kernel SIGKILLs a pid namespace's remaining members when its init
        # exits and a member cannot rejoin an ancestor namespace. Adopting that
        # is a HOST decision this section does not take: it re-parents every
        # phase, changes how exit status and `timeout` compose, and its
        # availability on the GitHub runner is unmeasured -- and a containment
        # that works locally while returning EPERM in CI would advertise a
        # property exactly where the leak is charged to the next job. So the
        # assertion below pins what ships, and flipping it is the visible half
        # of adopting a namespace backend later.
        g_s56_restore
        G_S56_RUN3="$TMP_DIR/s56-detach"
        rm -rf "$G_S56_RUN3"; mkdir -p "$G_S56_RUN3/tmp"
        G_S56_LOADER3="$TMP_DIR/s56-loader-detach.py"
        g_s56_loader "$G_S56_RUN3" ", start_new_session=True" ".pid" > "$G_S56_LOADER3"
        (cd "$GATE_REPO" || exit 1
         cat "$G_S56_LOADER3" >> scripts/todo-graph/cache_schema.py
         git commit --quiet --no-verify -am "22ev: a loader whose descendant leaves the group" >/dev/null 2>&1)
        (cd "$GATE_REPO" && TMPDIR="$G_S56_RUN3/tmp" bash "$GATE_IN_CLONE" \
            --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22ev.log" 2>&1)
        G_S56_RC3=$?
        g_s56_collect "$G_S56_RUN3"
        if [ -z "$G_S56_RAN" ]; then
            t_fail "identity gate: no phase reached the 22ev detaching loader, so the limit was not measured (rc=$G_S56_RC3; see $TMP_DIR/gate-22ev.log)"
        elif [ -n "$G_S56_LIVE" ]; then
            t_pass "identity gate: a descendant that LEAVES the group survives the reap ($G_S56_LIVE) -- the documented limit, measured"
        else
            t_fail "identity gate: a detached descendant did NOT survive, so either the loader failed to detach or the gate gained containment this case does not describe (ran:$G_S56_RAN; see $TMP_DIR/gate-22ev.log)"
        fi
        rm -rf "$G_S56_RUN3"
        g_s56_restore

        # ------------------------------------------------------------------
        # s60a: A MID-PHASE SUBSTITUTION IS DETECTED, NOT PUBLISHED THROUGH
        # (section 60).
        #
        # 22eu/22ev/22ew prove the REAP contains what a phase spawns. This
        # proves something narrower and newer: a detached descendant that
        # renames the gate's own temporary directory WHILE a supervised phase
        # is running is caught the moment that phase's reap runs, not merely
        # at the START of whatever phase happens to run next. Discovery is
        # through PYTHONPYCACHEPREFIX, which section 60's own tmp_path()
        # comment names as the one name this section deliberately still
        # exports -- the fixture exercises that documented route rather than
        # inventing an easier one.
        #
        # NOT A CLAIM THAT THE VERDICT WOULD HAVE BEEN WRONG WITHOUT THIS.
        # Item 1's descriptor-relative reads already make every operational
        # read and write in this window immune to the rename by construction
        # -- proven by every other identity-gate case in this file still
        # passing. What this adds is SURFACING the tampering attempt as a
        # loud refusal instead of the gate silently completing as if nothing
        # happened. The control below proves the refusal is THIS code, not
        # `phase_budget`'s older boundary check firing one phase later.
        g_s56_restore
        G_S60_RUN="$TMP_DIR/s60-swap"
        rm -rf "$G_S60_RUN"; mkdir -p "$G_S60_RUN/tmp"
        G_S60_LOADER="$TMP_DIR/s60-loader.py"
        {
            printf '\n\n'
            printf '# section 60: rename the gate own temporary directory away and leave a\n'
            printf '# fresh, empty one at the original name, from INSIDE the head cache build\n'
            printf '# phase -- the swap a detached descendant makes, discovered through the\n'
            printf '# one name section 60 deliberately still exports.\n'
            printf 'import os as _s60_os, sys as _s60_sys\n'
            printf 'def _s60_gate_own(_p):\n'
            printf '    return _p.startswith("/proc/self/fd/") or _s60_os.path.basename(_s60_os.path.dirname(_p)).startswith("identity-gate.")\n'
            printf 'def _s60_phase():\n'
            printf '    _a = [str(_x) for _x in _s60_sys.argv]\n'
            printf '    _p0 = _s60_os.path.basename(_a[0]) if _a else ""\n'
            printf '    if _p0 == "build.py":\n'
            printf '        for _i, _x in enumerate(_a):\n'
            printf '            if _x == "--output" and _i + 1 < len(_a):\n'
            printf '                _s60_p = _a[_i + 1]\n'
            printf '                if not _s60_gate_own(_s60_p):\n'
            printf '                    return ""\n'
            printf '                return "bcache" if _s60_os.path.basename(_s60_p) == "todo-cache-base.json" else "hcache"\n'
            printf '    return ""\n'
            printf 'if _s60_phase() == "hcache":\n'
            printf '    _pp = _s60_os.environ.get("PYTHONPYCACHEPREFIX", "")\n'
            printf '    if _pp.endswith("/pycache"):\n'
            printf '        _td = _pp[:-len("/pycache")]\n'
            printf '        try:\n'
            printf '            _s60_os.rename(_td, _td + ".s60bak")\n'
            printf '            _s60_os.mkdir(_td, 0o700)\n'
            printf "            open('%s/swapped', 'w').close()\n" "$G_S60_RUN"
            printf '        except OSError as _e:\n'
            printf "            open('%s/error', 'w').write(str(_e))\n" "$G_S60_RUN"
        } > "$G_S60_LOADER"
        (cd "$GATE_REPO" || exit 1
         cat "$G_S60_LOADER" >> scripts/todo-graph/cache_schema.py
         git commit --quiet --no-verify -am "s60: rename TMP_DIR from inside the head cache build phase" >/dev/null 2>&1)
        (cd "$GATE_REPO" && TMPDIR="$G_S60_RUN/tmp" bash "$GATE_IN_CLONE" \
            --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-s60a.log" 2>&1)
        G_S60_RC=$?
        if [ ! -e "$G_S60_RUN/swapped" ]; then
            t_fail "identity gate: the s60a loader never reached the head cache build phase, so the mid-phase swap was not attempted (rc=$G_S60_RC; error=$(cat "$G_S60_RUN/error" 2>/dev/null); see $TMP_DIR/gate-s60a.log)"
        elif [ "$G_S60_RC" -ne 3 ] || ! grep -q "discovered immediately after a supervised phase completed" "$TMP_DIR/gate-s60a.log"; then
            t_fail "identity gate: a mid-phase TMP_DIR swap did not produce the post-reap anchor refusal (rc=$G_S60_RC; see $TMP_DIR/gate-s60a.log)"
        else
            t_pass "identity gate: a detached descendant's mid-phase TMP_DIR swap is refused at the very next reap, not merely at the next phase boundary"

            # CONTROL: strip the section 60 post-reap check and require the SAME
            # fixture to lose the SPECIFIC refusal it just produced -- proving
            # this test measures the new code, not `phase_budget`'s older,
            # slower-firing boundary check (untouched, and would still
            # eventually catch the same swap one phase later, with different
            # text).
            G_S60_MUT="$GATE_REPO/scripts/todo-graph/identity-gate-s60mut.sh"
            python3 - "$GATE_IN_CLONE" "$G_S60_MUT" <<'S60MUTPY'
import sys
src = open(sys.argv[1], encoding="utf-8").read()
marker = "discovered immediately after a supervised phase completed"
mi = src.find(marker)
if mi < 0:
    raise SystemExit("s60 mutation: marker text not found")
lo = src.rfind('\n    if [ -n "${TMP_FD:-}" ] && ! tmp_dir_anchor_ok; then\n', 0, mi)
if lo < 0:
    raise SystemExit("s60 mutation: enclosing if-line not found")
lo += 1  # keep the newline that precedes it as the block's own leading break
fi = src.find("\n    fi\n", mi)
if fi < 0:
    raise SystemExit("s60 mutation: enclosing fi-line not found")
fi_end = fi + len("\n    fi\n")
mutated = src[:lo] + src[fi_end:]
if mutated == src:
    raise SystemExit("s60 mutation: no bytes removed")
open(sys.argv[2], "w", encoding="utf-8").write(mutated)
S60MUTPY
            if [ -s "$G_S60_MUT" ]; then
                chmod +x "$G_S60_MUT"
                g_s56_restore
                rm -rf "$G_S60_RUN"; mkdir -p "$G_S60_RUN/tmp"
                (cd "$GATE_REPO" || exit 1
                 cat "$G_S60_LOADER" >> scripts/todo-graph/cache_schema.py
                 git commit --quiet --no-verify -am "s60: rename TMP_DIR from inside the head cache build phase (control)" >/dev/null 2>&1)
                (cd "$GATE_REPO" && TMPDIR="$G_S60_RUN/tmp" bash "$G_S60_MUT" \
                    --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-s60a-mut.log" 2>&1)
                if grep -q "discovered immediately after a supervised phase completed" "$TMP_DIR/gate-s60a-mut.log"; then
                    t_fail "identity gate: s60a's refusal survived removing the section 60 post-reap check, so the case proves nothing about that code (see $TMP_DIR/gate-s60a-mut.log)"
                else
                    t_pass "identity gate: MUTATION -- without the post-reap anchor check the same swap loses this specific refusal (phase_budget's older boundary check may still catch it later; see $TMP_DIR/gate-s60a-mut.log)"
                fi
                # NOT COMMITTED, SO IT MUST BE REMOVED BY HAND -- unlike the
                # loader injections above (which land through a commit
                # `g_s56_restore` checks out away), this mutation copy is a
                # plain file write inside $GATE_REPO with no commit at all, and
                # every later fixture in this file assumes a clean working
                # tree there (measured: left in place, it read as untracked
                # dirt to 22ee's and 22ek's own cleanliness checks).
                rm -f "$G_S60_MUT"
            else
                t_fail "identity gate: could not build the s60a mutation copy of the gate"
            fi
        fi
        rm -rf "$G_S60_RUN"
        g_s56_restore

        # ------------------------------------------------------------------
        # s60b: THE HELPERS THEMSELVES RESOLVE THROUGH THE DESCRIPTOR, NOT
        # MERELY DETECT A RENAME AFTER IT (Codex test-coverage review,
        # section 60). s60a proves the post-reap DETECTOR fires; it does not
        # prove `tmp_path()`/`base_tree_path()`/`head_tree_path()` are what
        # makes detection necessary in the first place -- reverting any of
        # the three to their pre-section-60 name-based form would still pass
        # s60a unchanged, because that case never inspects WHAT was read.
        #
        # DETERMINISTIC, NOT RACED. Racing a live phase against a detached
        # descendant (what s60a and 22ev both do) is unavoidable for testing
        # DETECTION, which only exists at a boundary a live run reaches; it
        # is not needed here, because a helper's resolution rule is a pure
        # function of its inputs. This extracts the three functions VERBATIM
        # from the shipped gate, anchors a descriptor on a REAL directory,
        # renames it away and puts a DECOY with different content at the old
        # name, then calls the helper directly and reads back what it
        # resolved to -- no phase, no timing window, no flake surface.
        g_s60b_extract() {   # $1 = source gate file -> the four functions, verbatim
            python3 - "$1" <<'S60BEXTPY'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
out = []
for name in ("die_infra", "tmp_path", "base_tree_path", "head_tree_path"):
    m = re.search(r'^' + name + r'\(\) \{.*?\n\}\n', src, re.DOTALL | re.MULTILINE)
    if not m:
        raise SystemExit("s60b: could not extract " + name + "()")
    out.append(m.group(0))
sys.stdout.write("\n".join(out))
S60BEXTPY
        }
        G_S60B_HELPERS="$(g_s60b_extract "$GATE_IN_CLONE")" \
            || t_fail "identity gate: could not extract tmp_path()/base_tree_path()/head_tree_path() from the shipped gate for isolated testing"
        if [ -n "$G_S60B_HELPERS" ]; then
            g_s60b_reset() {   # $1 = probe dir -> a fresh anchor with REAL CONTENT
                rm -rf "$1"; mkdir -p "$1/real"
                printf 'REAL CONTENT\n' > "$1/real/probe.txt"
            }
            g_s60b_probe() {   # $1 = helper name, $2 = fd var name -> t_pass/t_fail
                local _helper="$1" _fdvar="$2" _dir _out
                _dir="$TMP_DIR/s60b-$_helper"

                # SHIPPED SHAPE: the helper as this section actually wrote it.
                g_s60b_reset "$_dir"
                cat > "$TMP_DIR/s60b-$_helper-probe.sh" <<EOF
set -u
$G_S60B_HELPERS
exec {$_fdvar}<"$_dir/real"
mv "$_dir/real" "$_dir/real.bak"
mkdir "$_dir/real"
printf 'DECOY CONTENT\n' > "$_dir/real/probe.txt"
cat "\$($_helper probe.txt)"
EOF
                _out="$(bash "$TMP_DIR/s60b-$_helper-probe.sh" 2>"$TMP_DIR/s60b-$_helper.err")"
                if [ "$_out" != "REAL CONTENT" ]; then
                    t_fail "identity gate: $_helper() read '$_out' after the rename, not the anchored REAL CONTENT -- it is resolving by name (see $TMP_DIR/s60b-$_helper.err)"
                    return
                fi

                # CONTROL: the pre-section-60 shape of the SAME helper -- a
                # plain name join, no descriptor -- must read the DECOY
                # instead, or this case proves nothing about which shape is
                # in the shipped file.
                g_s60b_reset "$_dir"
                cat > "$TMP_DIR/s60b-$_helper-mut.sh" <<EOF
set -u
die_infra() { printf '[identity-gate] INFRASTRUCTURE: %s\n' "\$1" >&2; exit 3; }
$_helper() { printf '%s/%s' "$_dir/real" "\$1"; }
exec {$_fdvar}<"$_dir/real"
mv "$_dir/real" "$_dir/real.bak"
mkdir "$_dir/real"
printf 'DECOY CONTENT\n' > "$_dir/real/probe.txt"
cat "\$($_helper probe.txt)"
EOF
                _out="$(bash "$TMP_DIR/s60b-$_helper-mut.sh" 2>"$TMP_DIR/s60b-$_helper-mut.err")"
                if [ "$_out" != "DECOY CONTENT" ]; then
                    t_fail "identity gate: the pre-section-60 (name-based) shape of $_helper() did not read the decoy ('$_out'), so this case is not sensitive to the descriptor at all (see $TMP_DIR/s60b-$_helper-mut.err)"
                    return
                fi
                t_pass "identity gate: $_helper() resolves through its descriptor after a rename (reads REAL CONTENT); its pre-section-60 name-based shape reads the DECOY instead (mutation-proved)"
                rm -rf "$_dir"
            }
            g_s60b_probe tmp_path TMP_FD
            g_s60b_probe base_tree_path BASE_TREE_FD
            g_s60b_probe head_tree_path HEAD_TREE_FD
        fi

        # ------------------------------------------------------------------
        # 22ex: THE HELPER'S OWN BRANCHES, IN ISOLATION (section 56).
        #
        # 22eu/22ew drive `reap_walk_group` end to end, and they can only ever
        # reach its COOPERATIVE path: they hand it a freshly `wait`ed leader
        # whose descendant is an ordinary `sleep`, which dies on the first TERM.
        # Three branches are therefore invisible to them -- the recycled-leader
        # guard, the KILL escalation after a descendant ignores TERM, and the
        # deadline break in the grace loop -- and a mutation to any of them
        # stays green (Codex test-coverage, section 56, [high]).
        #
        # Driven directly rather than through the gate: the function is lifted
        # out of the file under test with its own `remaining()` stubbed, so the
        # three branches are reachable in about a second each instead of behind
        # an end-to-end gate invocation apiece. The extraction is anchored, so a
        # renamed function fails here rather than silently testing nothing.
        g_s56_harness() {   # $1 = gate file, $2 = remaining() answer, $3 = out path
            {
                echo 'set -uo pipefail'
                echo "remaining() { echo $2; }"
                sed -n '/^reap_walk_group() {/,/^}$/p' "$1"
                echo 'reap_walk_group "$@"'
            } > "$3"
            grep -q '^reap_walk_group() {' "$3" && grep -q '^    for _rg in' "$3" && bash -n "$3" 2>/dev/null
        }
        G_S56_HRUN="$TMP_DIR/s56-helper"
        rm -rf "$G_S56_HRUN"; mkdir -p "$G_S56_HRUN"
        G_S56_H="$G_S56_HRUN/harness.sh"
        if ! g_s56_harness "$GATE_IN_CLONE" 9999 "$G_S56_H"; then
            t_fail "identity gate: could not lift reap_walk_group out of the gate (the function head moved)"
        else
            # ARM 1: a leader that is STILL ALIVE means its pid was recycled, so
            # the group belongs to somebody else and must not be signalled.
            # THE LEADER REPORTS ITS OWN PID, never `$!`. `setsid` FORKS when
            # its caller is already a process-group leader, which is exactly
            # what happens when the harness shell has job control on -- `$!` is
            # then the short-lived `setsid` process and the real session leader
            # is a pid nobody recorded, so both arms below would pass for the
            # wrong reason (measured while building this case).
            setsid bash -c 'echo $$ > '"$G_S56_HRUN"'/ldr1; sleep 300 >/dev/null 2>&1 & echo $! > '"$G_S56_HRUN"'/kid1; sleep 300' >/dev/null 2>&1 &
            for _i in $(seq 1 100); do
                [ -s "$G_S56_HRUN/kid1" ] && [ -s "$G_S56_HRUN/ldr1" ] && break
                sleep 0.1
            done
            G_S56_L1="$(cat "$G_S56_HRUN/ldr1" 2>/dev/null)"
            G_S56_K1="$(cat "$G_S56_HRUN/kid1" 2>/dev/null)"
            if [ -z "$G_S56_K1" ] || [ -z "$G_S56_L1" ]; then
                t_fail "identity gate: the 22ex live-leader arm never started its descendant"
            else
                bash "$G_S56_H" "$G_S56_L1" >/dev/null 2>&1
                if kill -0 "$G_S56_K1" 2>/dev/null && kill -0 "$G_S56_L1" 2>/dev/null; then
                    t_pass "identity gate: a LIVE leader pid is treated as recycled, so its group is left alone"
                else
                    t_fail "identity gate: reap_walk_group signalled a live leader's group, which is how a recycled pid takes a stranger with it"
                fi
                # MUTATION: drop the leader-is-gone half of the guard and the
                # same call must now reap the stranger's group.
                G_S56_HM="$G_S56_HRUN/harness-noguard.sh"
                sed 's#^        if ! kill -0 "$_rg" 2>/dev/null && kill -0 -- "-$_rg" 2>/dev/null; then$#        if kill -0 -- "-$_rg" 2>/dev/null; then#' \
                    "$G_S56_H" > "$G_S56_HM"
                if grep -q '^        if ! kill -0 "\$_rg" 2>/dev/null && kill -0 -- "-\$_rg" 2>/dev/null; then$' "$G_S56_HM"; then
                    t_fail "identity gate: could not build the recycled-leader guard mutation (the shape moved)"
                else
                    bash "$G_S56_HM" "$G_S56_L1" >/dev/null 2>&1
                    if kill -0 "$G_S56_K1" 2>/dev/null; then
                        t_fail "identity gate: removing the leader-is-gone guard did not reap the group, so 22ex proves nothing about it"
                    else
                        t_pass "identity gate: MUTATION -- without the leader-is-gone guard a live pid's group IS signalled, so the guard is load-bearing"
                    fi
                fi
                g_s53_reap "$G_S56_K1" "$G_S56_L1"
            fi

            # ARM 2: a descendant that IGNORES TERM must still be gone, which is
            # the KILL escalation. The loop keeps re-entering `sleep` so the
            # group TERM cannot end it by killing the sleep alone.
            g_s56_stubborn() {   # $1 = pid file; ANSWERS the leader pgid in G_S56_LS
                rm -f "$G_S56_HRUN/ldrs"
                setsid bash -c 'echo $$ > '"$G_S56_HRUN"'/ldrs; bash -c '"'"'trap "" TERM; echo $$ > '"$1"'; while :; do sleep 1; done'"'"' >/dev/null 2>&1 & sleep 0.4' >/dev/null 2>&1 &
                for _i in $(seq 1 100); do
                    [ -s "$1" ] && [ -s "$G_S56_HRUN/ldrs" ] && break
                    sleep 0.1
                done
                G_S56_LS="$(cat "$G_S56_HRUN/ldrs" 2>/dev/null)"
                # The leader must be GONE before the helper is called: that is
                # the success-path ordering this whole section is about.
                for _i in $(seq 1 60); do
                    kill -0 "$G_S56_LS" 2>/dev/null || break
                    sleep 0.1
                done
            }
            g_s56_stubborn "$G_S56_HRUN/kid2"
            G_S56_K2="$(cat "$G_S56_HRUN/kid2" 2>/dev/null)"
            if [ -z "$G_S56_K2" ]; then
                t_fail "identity gate: the 22ex TERM-resistant arm never started its descendant"
            else
                bash "$G_S56_H" "$G_S56_LS" >/dev/null 2>&1
                if g_s56_gone "$G_S56_K2"; then
                    t_pass "identity gate: a descendant that ignores TERM is still gone, so the KILL escalation runs"
                else
                    t_fail "identity gate: a TERM-resistant descendant survived reap_walk_group, so the KILL escalation is not reached"
                fi
                G_S56_HK="$G_S56_HRUN/harness-nokill.sh"
                sed 's#^            kill -KILL -- "-$_rg" 2>/dev/null || true$#            :#' "$G_S56_H" > "$G_S56_HK"
                if grep -q '^            kill -KILL -- "-\$_rg" 2>/dev/null || true$' "$G_S56_HK"; then
                    t_fail "identity gate: could not build the KILL-escalation mutation (the shape moved)"
                else
                    g_s56_stubborn "$G_S56_HRUN/kid3"
                    G_S56_K3="$(cat "$G_S56_HRUN/kid3" 2>/dev/null)"
                    bash "$G_S56_HK" "$G_S56_LS" >/dev/null 2>&1
                    if [ -n "$G_S56_K3" ] && kill -0 "$G_S56_K3" 2>/dev/null; then
                        t_pass "identity gate: MUTATION -- without the KILL escalation the TERM-resistant descendant survives"
                    else
                        t_fail "identity gate: removing the KILL escalation did not leak, so 22ex proves nothing about it"
                    fi
                    g_s53_reap "$G_S56_K3"
                fi
                g_s53_reap "$G_S56_K2"
            fi

            # ARM 3: THE GRACE LOOP ANSWERS TO THE DEADLINE. With `remaining()`
            # at 1 the loop must break on its FIRST turn rather than spend six
            # half-second sleeps -- eleven sites at 3s each is how a bounded
            # gate stops being bounded.
            #
            # COUNTED, NOT TIMED. The first cut asserted a wall-clock delta on
            # `date +%s`, which is exactly the shape section 34 of this TODO
            # forbids: a clock STEP can satisfy or break a budget assertion that
            # measures nothing about the code (Codex perf, section 56,
            # [medium]). The harness instead shadows `sleep` with a counter, so
            # the assertion is on the number of grace turns the loop actually
            # takes -- deterministic under any load, and it also removes the 3s
            # the mutated arm used to spend really sleeping.
            G_S56_CNT="$G_S56_HRUN/graceturns"
            g_s56_counting_harness() {   # $1 = gate file, $2 = remaining(), $3 = out
                {
                    echo 'set -uo pipefail'
                    echo "remaining() { echo $2; }"
                    echo "sleep() { echo x >> \"\$G_S56_CNT\"; }"
                    sed -n '/^reap_walk_group() {/,/^}$/p' "$1"
                    echo 'reap_walk_group "$@"'
                } > "$3"
                grep -q '^reap_walk_group() {' "$3" && bash -n "$3" 2>/dev/null
            }
            export G_S56_CNT
            G_S56_H1="$G_S56_HRUN/harness-counting.sh"
            if ! g_s56_counting_harness "$GATE_IN_CLONE" 1 "$G_S56_H1"; then
                t_fail "identity gate: could not build the counting reap harness (the function head moved)"
            else
                g_s56_stubborn "$G_S56_HRUN/kid4"
                G_S56_K4="$(cat "$G_S56_HRUN/kid4" 2>/dev/null)"
                : > "$G_S56_CNT"
                bash "$G_S56_H1" "$G_S56_LS" >/dev/null 2>&1
                G_S56_N1=$(wc -l < "$G_S56_CNT" 2>/dev/null | tr -d ' ')
                g_s53_reap "$G_S56_K4"
                G_S56_HB="$G_S56_HRUN/harness-nobreak.sh"
                grep -v -- '-gt 1 \] || break' "$G_S56_H1" > "$G_S56_HB"
                if grep -q 'remaining' "$G_S56_HB" && ! grep -q -- '-gt 1 \] || break' "$G_S56_HB"; then
                    g_s56_stubborn "$G_S56_HRUN/kid5"
                    G_S56_K5="$(cat "$G_S56_HRUN/kid5" 2>/dev/null)"
                    : > "$G_S56_CNT"
                    bash "$G_S56_HB" "$G_S56_LS" >/dev/null 2>&1
                    G_S56_N2=$(wc -l < "$G_S56_CNT" 2>/dev/null | tr -d ' ')
                    g_s53_reap "$G_S56_K5"
                    if [ "$G_S56_N1" -eq 0 ] && [ "$G_S56_N2" -ge 6 ]; then
                        t_pass "identity gate: an expired budget breaks the grace loop before its first sleep (${G_S56_N1} turns), and removing the break spends every turn (${G_S56_N2})"
                    else
                        t_fail "identity gate: the grace loop's deadline break is not measurable (shipped=${G_S56_N1} turns mutated=${G_S56_N2}; want 0 and >= 6)"
                    fi
                else
                    t_fail "identity gate: could not build the grace-loop deadline mutation (the shape moved)"
                fi
            fi
            g_s53_reap "$G_S56_LS"
        fi
        rm -rf "$G_S56_HRUN"

        # ------------------------------------------------------------------
        # 22ey/22ez: THE ANCHOR REFUSAL AND THE CLEANUP THAT MUST NOT DELETE
        # WHAT IT DID NOT CREATE (section 56).
        #
        # Nothing else in the suite substitutes the gate's own temporary
        # directory, so both halves of the TMP_DIR identity work -- the
        # phase-boundary refusal and cleanup removing what the DESCRIPTOR holds
        # -- could regress green (Codex test-coverage, section 56, [high]).
        # The loader derives TMP_DIR from the exported `PYTHONPYCACHEPREFIX`,
        # which is the same route a hostile tree would take and is documented as
        # such in `proto_of`'s own comment.
        g_s56_swap_loader() {   # $1 = mode (rename|delete), $2 = marker path
            # THE CONTENTS MOVE WITH IT, which is what makes this a test of the
            # IDENTITY rather than of a broken run. A bare rename leaves the
            # phase writing into an EMPTY replacement, so the phase fails on its
            # own and the gate refuses for that reason instead -- at which point
            # disarming the anchor check changes nothing and the mutation proves
            # nothing (measured while building this case: the run refused with
            # "cache build failed", not with the boundary diagnostic). Moving
            # every entry back leaves the substituted directory complete and
            # USABLE, so the only thing wrong with it is that it is a different
            # inode from the one the gate anchored -- the property under test.
            printf '\n\n'
            printf '# section 56: substitute the gate own temporary directory from inside a phase.\n'
            printf 'import os as _s56s_os, shutil as _s56s_sh, sys as _s56s_sys\n'
            printf '_s56s_pc = _s56s_os.environ.get("PYTHONPYCACHEPREFIX", "")\n'
            printf '_s56s_td = _s56s_os.path.dirname(_s56s_pc) if _s56s_pc else ""\n'
            printf '_s56s_go = _s56s_os.path.basename(_s56s_td).startswith("identity-gate.")\n'
            printf '_s56s_me = _s56s_os.path.basename(_s56s_sys.argv[0]) if _s56s_sys.argv else ""\n'
            printf 'if _s56s_go and _s56s_me == "build.py" and not _s56s_os.path.exists("%s"):\n' "$2"
            printf '    try:\n'
            printf '        _s56s_old = _s56s_td + ".moved"\n'
            printf '        _s56s_os.rename(_s56s_td, _s56s_old)\n'
            printf '        _s56s_os.mkdir(_s56s_td)\n'
            printf '        for _s56s_n in _s56s_os.listdir(_s56s_old):\n'
            printf '            _s56s_os.rename(_s56s_os.path.join(_s56s_old, _s56s_n),\n'
            printf '                            _s56s_os.path.join(_s56s_td, _s56s_n))\n'
            printf "        open(_s56s_os.path.join(_s56s_td, 'REPLACEMENT'), 'w').write('not the gate')\n"
            if [ "$1" = "delete" ]; then
                # AND THE ANCHORED INODE IS DESTROYED, which is the OTHER
                # cleanup branch: the descriptor then resolves to nothing, so
                # there is nothing for the gate to remove and the replacement
                # must survive untouched rather than be taken for the original.
                printf '        _s56s_sh.rmtree(_s56s_old)\n'
            fi
            printf "        open('%s', 'w').write(_s56s_td)\n" "$2"
            printf '    except OSError:\n'
            printf '        pass\n'
        }
        for G_S56_MODE in rename delete; do
            g_s56_restore
            G_S56_SRUN="$TMP_DIR/s56-swap-$G_S56_MODE"
            rm -rf "$G_S56_SRUN"; mkdir -p "$G_S56_SRUN/tmp"
            G_S56_MARK="$G_S56_SRUN/swapped"
            g_s56_swap_loader "$G_S56_MODE" "$G_S56_MARK" > "$TMP_DIR/s56-swap.py"
            (cd "$GATE_REPO" || exit 1
             cat "$TMP_DIR/s56-swap.py" >> scripts/todo-graph/cache_schema.py
             git commit --quiet --no-verify -am "22e$G_S56_MODE: substitute TMP_DIR from inside a phase" >/dev/null 2>&1)
            (cd "$GATE_REPO" && TMPDIR="$G_S56_SRUN/tmp" bash "$GATE_IN_CLONE" \
                --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22e-$G_S56_MODE.log" 2>&1)
            G_S56_SRC=$?
            G_S56_SWAPPED="$(cat "$G_S56_MARK" 2>/dev/null)"
            if [ -z "$G_S56_SWAPPED" ]; then
                t_fail "identity gate: the $G_S56_MODE loader never substituted TMP_DIR, so the case measured nothing (rc=$G_S56_SRC; see $TMP_DIR/gate-22e-$G_S56_MODE.log)"
            elif [ "$G_S56_SRC" -ne 3 ]; then
                t_fail "identity gate: a substituted TMP_DIR did not refuse the run (rc=$G_S56_SRC, want 3; see $TMP_DIR/gate-22e-$G_S56_MODE.log)"
            elif [ ! -e "$G_S56_SWAPPED/REPLACEMENT" ]; then
                t_fail "identity gate: cleanup DELETED the replacement directory it did not create ($G_S56_SWAPPED); this is the failure the descriptor-resolved removal exists to prevent"
            else
                t_pass "identity gate: a TMP_DIR substituted mid-phase ($G_S56_MODE) refuses at the next boundary and cleanup leaves the replacement alone"
            fi
            if [ "$G_S56_MODE" = "rename" ] && [ -n "$G_S56_SWAPPED" ]; then
                if [ -d "$G_S56_SWAPPED.moved" ]; then
                    t_fail "identity gate: cleanup leaked the anchored directory after it was renamed ($G_S56_SWAPPED.moved survived)"
                else
                    t_pass "identity gate: cleanup removed what the descriptor holds, so a renamed anchored directory is not leaked"
                fi
                # MUTATION: disarm the phase-boundary anchor check and the run
                # must reach a verdict over the substituted directory instead of
                # refusing.
                G_S56_SMUT="$GATE_REPO/scripts/todo-graph/identity-gate-noanchor.sh"
                sed 's#^    if \[ \-n "${TMP_FD:-}" \] && ! tmp_dir_anchor_ok; then$#    if false; then#' \
                    "$GATE_IN_CLONE" > "$G_S56_SMUT"
                if ! g_mutant_ok "$GATE_IN_CLONE" "$G_S56_SMUT" 'tmp_dir_anchor_ok' \
                   || grep -q '^    if \[ -n "${TMP_FD:-}" \] && ! tmp_dir_anchor_ok; then$' "$G_S56_SMUT"; then
                    t_fail "identity gate: could not build the phase-boundary anchor mutation (the shape moved)"
                else
                    rm -f "$G_S56_MARK"
                    rm -rf "$G_S56_SRUN/tmp"; mkdir -p "$G_S56_SRUN/tmp"
                    (cd "$GATE_REPO" && TMPDIR="$G_S56_SRUN/tmp" bash "$G_S56_SMUT" \
                        --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22ey-mut.log" 2>&1)
                    G_S56_MRC=$?
                    if [ -s "$G_S56_MARK" ] && [ "$G_S56_MRC" -ne 3 ]; then
                        t_pass "identity gate: MUTATION -- without the phase-boundary anchor check the run continues past a substituted TMP_DIR (rc=$G_S56_MRC)"
                    else
                        t_fail "identity gate: disarming the anchor check still refused (rc=$G_S56_MRC), so 22ey does not measure that check (see $TMP_DIR/gate-22ey-mut.log)"
                    fi
                fi
                rm -f "$G_S56_SMUT"
            fi
            # GUARDED ON A NON-EMPTY VALUE: an unset `G_S56_SWAPPED` would make
            # the second and third arguments `""` and `".moved"`, and the
            # relative one resolves against the repository root.
            rm -rf "$G_S56_SRUN" 2>/dev/null
            [ -n "$G_S56_SWAPPED" ] && rm -rf "$G_S56_SWAPPED" "$G_S56_SWAPPED.moved" 2>/dev/null
            g_s56_restore
        done

        # ------------------------------------------------------------------
        # 22f0: THE ORDERING, STRUCTURALLY (section 56).
        #
        # The claim every call site rests on is that `WALK_PIDS` is cleared only
        # AFTER the reap, so a trap firing mid-reap still finds a target. A
        # timing-raced fixture for that would have to land a signal inside a
        # sub-second window in a suite that already carries documented
        # wall-clock flakes; the ordering is a STATIC property of the eleven
        # sites, so it is asserted statically instead -- and that also catches a
        # twelfth site added later without a reap at all.
        G_S56_ORD="$(cd "$GATE_REPO" && python3 - "$GATE_IN_CLONE" <<'S56ORD2'
import re, sys
lines = open(sys.argv[1], encoding="utf-8").read().split("\n")
sites, bad = [], []
for i, ln in enumerate(lines):
    if ln.lstrip().startswith("#"):
        continue
    m = re.search(r'WALK_WAITED="\$([A-Za-z_][A-Za-z0-9_]*)"', ln)
    if not m:
        continue
    var = m.group(1)
    sites.append((i + 1, var))
    reaped = False
    for j in range(i + 1, min(i + 12, len(lines))):
        nxt = lines[j]
        if nxt.lstrip().startswith("#"):
            continue
        if re.search(r'reap_walk_group "\$%s"' % re.escape(var), nxt):
            reaped = True
        if re.match(r'^\s*WALK_PIDS=""\s*$', nxt):
            break
    if not reaped:
        bad.append((i + 1, var))
print("%d|%s" % (len(sites), ",".join("%d:%s" % b for b in bad)))
S56ORD2
)"
        G_S56_NSITES="${G_S56_ORD%%|*}"; G_S56_BAD="${G_S56_ORD#*|}"
        if [ "$G_S56_NSITES" != "11" ]; then
            t_fail "identity gate: expected 11 supervised-phase wait sites, found $G_S56_NSITES -- a phase was added or removed without updating this assertion"
        elif [ -n "$G_S56_BAD" ]; then
            t_fail "identity gate: these waited leaders are untracked before they are reaped, so a trap landing mid-reap finds nothing: $G_S56_BAD"
        else
            t_pass "identity gate: all 11 waited leaders are reaped BEFORE WALK_PIDS is cleared, so the reap window stays covered by the trap"
        fi
    fi

    # ------------------------------------------------------------------
    # 22ee..22eh: WHICH TREE THE HEAD SIDE READS (section 54).
    #
    # Until section 54 every head-side read came from the LIVE WORKING TREE
    # while the verdict was published about HEAD. A dirty checkout could
    # therefore satisfy every check while the commit being pushed failed them,
    # and CI would reject that same commit afterwards on a clean tree. These
    # cases pin the head side to the COMMIT, and pin the two things that pins
    # cannot cover: the driver, which the hook executes from the working tree
    # and so cannot be redirected, and the base-first ordering the new
    # materialization sits inside.
    # ------------------------------------------------------------------
    g_s53_restore 2>/dev/null || true
    (cd "$GATE_REPO" && git checkout --quiet -- . 2>/dev/null)
    G_S54_BASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    # PREMISE: the base really does carry a protocol this gate can read, so a
    # failure below is about the head side and not about a fixture that walked
    # in with a broken base.
    G_S54_SEED_OK=no
    (cd "$GATE_REPO" && python3 -c 'import json,sys; json.load(open(sys.argv[1]))' \
        scripts/todo-graph/snapshot_protocol.json) >/dev/null 2>&1 && G_S54_SEED_OK=yes
    if [ "$G_S54_SEED_OK" != yes ]; then
        t_fail "identity gate: section 54 fixtures cannot start -- the seeded base protocol does not parse"
    else
        # 22ee: THE SECTION'S OWN TEST CHECKPOINT. Commit a protocol the loader
        # refuses TOGETHER with an unrelated closure change (so the
        # byte-identical exit does not apply), then restore a VALID protocol in
        # the worktree only. Before section 54 the head read succeeded on the
        # restored bytes and the run continued; now it reports the committed
        # protocol by name.
        (
            cd "$GATE_REPO" || exit 1
            python3 - scripts/todo-graph/snapshot_protocol.json <<'PY2'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text(encoding="utf-8"))
d["snapshot_schema"] = 0
p.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")
PY2
            printf '\n# 22ee: an unrelated closure edit, so the byte-identical exit does not apply\n' \
                >> scripts/todo-graph/resolve_symbol.py
            git commit --quiet --no-verify -am "22ee: a protocol the loader refuses, plus a closure edit" >/dev/null 2>&1
        )
        G_S54_HEAD="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
        # THE DIRTY RESTORE: valid protocol bytes on disk, broken ones in the
        # commit. This is the attack the section exists to close.
        (cd "$GATE_REPO" && git checkout --quiet "$G_S54_BASE" -- scripts/todo-graph/snapshot_protocol.json)
        G_S54_WT_OK=no
        (cd "$GATE_REPO" && python3 -c 'import json,sys; sys.exit(0 if json.load(open(sys.argv[1]))["snapshot_schema"] != 0 else 1)' \
            scripts/todo-graph/snapshot_protocol.json) >/dev/null 2>&1 && G_S54_WT_OK=yes
        G_S54_COMMIT_BAD=no
        (cd "$GATE_REPO" && git show "$G_S54_HEAD:scripts/todo-graph/snapshot_protocol.json" \
            | python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin)["snapshot_schema"] == 0 else 1)') \
            >/dev/null 2>&1 && G_S54_COMMIT_BAD=yes
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_S54_BASE" --head HEAD \
            >"$TMP_DIR/gate-22ee.log" 2>&1)
        G_RC=$?
        G_S54_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22ee.log" | head -1)"
        # THE PREMISES ARE ASSERTED, NOT ASSUMED: the worktree must really hold
        # the good bytes and the commit the bad ones, or this case proves
        # nothing about which of the two was read.
        if [ "$G_S54_WT_OK" = yes ] && [ "$G_S54_COMMIT_BAD" = yes ] \
           && [ "$G_RC" -eq 3 ] \
           && grep -q 'HEAD_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22ee.log" \
           && ! grep -q 'resolver closure byte-identical base\.\.head' "$TMP_DIR/gate-22ee.log"; then
            t_pass "identity gate: a dirty worktree cannot restore a protocol the pushed commit breaks"
        else
            t_fail "identity gate: the head side read the working tree (rc=$G_RC; got [$G_S54_TOKEN]; wt-good=$G_S54_WT_OK commit-bad=$G_S54_COMMIT_BAD; see $TMP_DIR/gate-22ee.log)"
        fi
        # AND THE DIVERGENCE IS DISCLOSED BY NAME. A verdict about a commit,
        # produced from a tree that differs from it, must say so rather than
        # leave a reader to infer it from the invocation.
        if grep -q 'HEAD_WORKTREE_DIVERGES' "$TMP_DIR/gate-22ee.log" \
           && grep -q 'diverges: scripts/todo-graph/snapshot_protocol.json' "$TMP_DIR/gate-22ee.log"; then
            t_pass "identity gate: an unadjudicated working-tree divergence is named, not inferred"
        else
            t_fail "identity gate: the run adjudicated the commit but never said the tree differed (see $TMP_DIR/gate-22ee.log)"
        fi
        # THE RECONCILIATION HALF: a CLEAN checkout of the same commit must
        # reach the SAME verdict. Equal tokens are what proves the two sources
        # were reconciled rather than one of them silently preferred.
        # THE WHOLE TREE, not just the file this case dirtied. Earlier fixtures
        # leave their own residue in the clone, and a "clean checkout" that is
        # only clean in one path would still report a divergence -- which is a
        # true statement about the tree and a false one about this case.
        # `HEAD --` RATHER THAN A BARE `--`: this case staged its restore with
        # `git checkout <base> -- <path>`, which writes the INDEX as well as the
        # worktree, so a bare `git checkout -- .` copies that staged version
        # straight back and the tree is never clean.
        (cd "$GATE_REPO" && git checkout --quiet HEAD -- . >/dev/null 2>&1)
        G_S54_DIRT="$( (cd "$GATE_REPO" && git status --porcelain --untracked-files=all | head -5) )"
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_S54_BASE" --head HEAD \
            >"$TMP_DIR/gate-22ee-clean.log" 2>&1)
        G_S54_CLEAN_RC=$?
        G_S54_CLEAN_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22ee-clean.log" | head -1)"
        if [ "$G_S54_CLEAN_RC" -eq "$G_RC" ] && [ -n "$G_S54_CLEAN_TOKEN" ] \
           && [ "$G_S54_CLEAN_TOKEN" = "$G_S54_TOKEN" ] \
           && ! grep -q 'HEAD_WORKTREE_DIVERGES' "$TMP_DIR/gate-22ee-clean.log"; then
            t_pass "identity gate: a clean checkout of the same commit reaches the same verdict"
        else
            t_fail "identity gate: dirty and clean checkouts of one commit disagree (dirty rc=$G_RC [$G_S54_TOKEN]; clean rc=$G_S54_CLEAN_RC [$G_S54_CLEAN_TOKEN]; residual dirt [$G_S54_DIRT]; see $TMP_DIR/gate-22ee-clean.log)"
        fi

        # 22ef: MUTATION -- put the head protocol read back on the working
        # tree. Without this the case cannot tell "the gate reads the commit"
        # from "the restore never happened".
        # THE SHARED CONTROL, not a second copy of the same sed. 22as builds the
        # identical mutation for the inverted cases; one helper means the two
        # cannot drift into disagreeing about what "the old behaviour" was.
        G_MUT_S54="$GATE_REPO/scripts/todo-graph/identity-gate-s54wt.sh"
        if ! g_s54_wtgate "$G_MUT_S54"; then
            t_fail "identity gate: could not build the working-tree-read mutation (the shape moved)"
        else
            (cd "$GATE_REPO" && git checkout --quiet "$G_S54_BASE" -- scripts/todo-graph/snapshot_protocol.json)
            (cd "$GATE_REPO" && bash "$G_MUT_S54" --base "$G_S54_BASE" --head HEAD \
                >"$TMP_DIR/gate-22ef-mut.log" 2>&1)
            G_S54_MUT_RC=$?
            # THE MUTANT'S OWN TERMINAL VERDICT IS ASSERTED, not "the log is not
            # empty". Absence of a token is satisfied by a run that never
            # started, and a non-empty log is satisfied by ANY early
            # infrastructure failure -- the same false positive one step weaker
            # (Codex adversarial, section 55 round 3, [medium]).
            #
            # THE MARKER IS "IT GOT PAST THE PROTOCOL CONTRACT", which is the
            # only thing this mutation is about. A first attempt pinned rc 0 on
            # the fast-path exit and was WRONG about this fixture: earlier cases
            # in this block append to `resolve_symbol.py`, so the range is not
            # byte-identical and the mutant correctly runs the differential --
            # where it then fails, because the COMMIT's protocol really is broken
            # and the walks execute the commit. That later failure is not this
            # case's subject. Reaching the materialized checkout is emitted only
            # after the head protocol contract has been satisfied, so it says
            # exactly what is meant: the restored working-tree bytes were read
            # and accepted.
            if grep -q 'walked from a materialized checkout' "$TMP_DIR/gate-22ef-mut.log" \
               && ! grep -qE 'HEAD_PROTOCOL_[A-Z_]+' "$TMP_DIR/gate-22ef-mut.log"; then
                t_pass "identity gate: MUTATION -- reading the worktree lets the restored bytes past the head protocol contract, so 22ee measures the source"
            else
                t_fail "identity gate: the working-tree-read mutation never got past the head protocol contract (rc=$G_S54_MUT_RC; see $TMP_DIR/gate-22ef-mut.log)"
            fi
            (cd "$GATE_REPO" && git checkout --quiet -- scripts/todo-graph/snapshot_protocol.json)
            rm -f "$G_MUT_S54"
        fi

        # 22eg: THE ONE HEAD-SIDE READ THAT CANNOT BE REDIRECTED. The hook
        # executes the working-tree driver, so a driver that differs from its
        # own commit is certifying a range with code that range does not carry.
        # It refuses rather than re-executing the committed copy, because
        # re-executing would change how the gate is INVOKED (section 16's
        # surface). The recovery half is asserted too: a refusal with no
        # demonstrated way out is a trap.
        (
            cd "$GATE_REPO" || exit 1
            printf '\n# 22eg: an uncommitted edit to the running driver\n' \
                >> scripts/todo-graph/identity-gate.sh
        )
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_S54_BASE" --head HEAD \
            >"$TMP_DIR/gate-22eg.log" 2>&1)
        G_S54_DRV_RC=$?
        (cd "$GATE_REPO" && git checkout --quiet -- scripts/todo-graph/identity-gate.sh)
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_S54_BASE" --head HEAD \
            >"$TMP_DIR/gate-22eg-recover.log" 2>&1)
        if [ "$G_S54_DRV_RC" -eq 3 ] \
           && grep -q 'GATE_DRIVER_NOT_AT_HEAD' "$TMP_DIR/gate-22eg.log" \
           && ! grep -q 'GATE_DRIVER_NOT_AT_HEAD' "$TMP_DIR/gate-22eg-recover.log"; then
            t_pass "identity gate: an uncommitted driver edit refuses, and committing it back clears"
        else
            t_fail "identity gate: the running driver was not checked against its own commit (rc=$G_S54_DRV_RC; see $TMP_DIR/gate-22eg.log)"
        fi

        # 22eh: BASE-FIRST STILL HOLDS ACROSS THE NEW MATERIALIZATION. The head
        # checkout is a head-side ACQUISITION, and base-first governs
        # classification, so a base this gate can classify must report even
        # when the head materialization fails outright. Forced with a mutation
        # rather than by contriving a disk failure, because the property under
        # test is the ORDERING, not the cause.
        G_MUT_S54B="$GATE_REPO/scripts/todo-graph/identity-gate-nohead.sh"
        sed 's#^    git -c "core.hooksPath=\$NOHOOKS_DIR" worktree add --detach "\$HEAD_TREE" "\$HEAD_RESOLVED" >/dev/null 2>&1 \\$#    false \\#' \
            "$GATE_IN_CLONE" > "$G_MUT_S54B"
        if cmp -s "$GATE_IN_CLONE" "$G_MUT_S54B"; then
            t_fail "identity gate: could not build the head-materialization-failure mutation (the shape moved; the mutant is identical to its source)"
        else
            # A base the gate can CLASSIFY: its own history added the protocol
            # files and this commit removes them.
            (
                cd "$GATE_REPO" || exit 1
                git rm --quiet -f scripts/todo-graph/snapshot_protocol.json \
                    scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
                git commit --quiet --no-verify -m "22eh: a base with the protocol removed" >/dev/null 2>&1
            )
            G_S54_BADBASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
            (
                cd "$GATE_REPO" || exit 1
                git checkout --quiet "$G_S54_BASE" -- scripts/todo-graph/snapshot_protocol.json \
                    scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
                printf '\n# 22eh: a closure edit on top of the restored protocol\n' \
                    >> scripts/todo-graph/resolve_symbol.py
                git commit --quiet --no-verify -am "22eh: restore the protocol and edit the closure" >/dev/null 2>&1
            )
            (cd "$GATE_REPO" && bash "$G_MUT_S54B" --base "$G_S54_BADBASE" --head HEAD \
                >"$TMP_DIR/gate-22eh.log" 2>&1)
            G_S54_ORD_RC=$?
            G_S54_ORD_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22eh.log" | head -1)"
            if [ "$G_S54_ORD_RC" -eq 3 ] \
               && grep -qE 'BASE_PROTOCOL_(REMOVED|INCOMPLETE|UNREADABLE)|BASE_PREDATES_PROTOCOL' "$TMP_DIR/gate-22eh.log" \
               && ! grep -q 'cannot materialize the head worktree' "$TMP_DIR/gate-22eh.log"; then
                t_pass "identity gate: a classifiable base still reports ahead of a failed head materialization"
            else
                t_fail "identity gate: head materialization jumped the base-first queue (rc=$G_S54_ORD_RC; got [$G_S54_ORD_TOKEN]; see $TMP_DIR/gate-22eh.log)"
            fi
            # CONTROL: the same mutation against a base the gate CANNOT
            # classify must reach the head materialization failure, or the case
            # above would pass on a gate that never runs the checkout at all.
            (cd "$GATE_REPO" && bash "$G_MUT_S54B" --base "$G_S54_BASE" --head HEAD \
                >"$TMP_DIR/gate-22eh-control.log" 2>&1)
            if grep -q 'cannot materialize the head worktree' "$TMP_DIR/gate-22eh-control.log"; then
                t_pass "identity gate: CONTROL -- with a clean base the forced head-checkout failure does surface"
            else
                t_fail "identity gate: the head-materialization mutation never fired, so 22eh proves nothing (see $TMP_DIR/gate-22eh-control.log)"
            fi
        fi
        # UNCONDITIONALLY, including on the build-failure path above. Left
        # inside the `else`, a mutation this case could not build stayed in the
        # clone as an untracked file -- and 22ek's premise is that the working
        # tree is CLEAN, so one case's failure produced a second, unrelated
        # failure two cases later.
        rm -f "$G_MUT_S54B"

        # 22ei: NO HOOK FROM THE TREE UNDER TEST RUNS DURING MATERIALIZATION.
        # `core.hooksPath` in the real repository is the RELATIVE path
        # `.githooks`, so it resolves inside whatever worktree git is
        # populating, and `git worktree add` runs `post-checkout` once the files
        # are there. A head commit carrying an executable `.githooks/post-checkout`
        # would therefore execute arbitrary code from the tree under test, before
        # any check, with the whole corpus in reach. The clone is configured the
        # same way ON PURPOSE -- a fixture that left hooks at `.git/hooks` would
        # test a repository this one is not.
        G_S54_HOOKMARK="$TMP_DIR/s54-post-checkout-ran"
        rm -f "$G_S54_HOOKMARK"
        (
            cd "$GATE_REPO" || exit 1
            git checkout --quiet HEAD -- . >/dev/null 2>&1
            mkdir -p .githooks
            {
                printf '#!/usr/bin/env bash\n'
                printf 'printf ran > %s\n' "$G_S54_HOOKMARK"
                printf 'rm -f todo/*/TODO-*.md 2>/dev/null || true\n'
                printf 'exit 0\n'
            } > .githooks/post-checkout
            chmod +x .githooks/post-checkout
            git add .githooks/post-checkout >/dev/null 2>&1
            git update-index --chmod=+x .githooks/post-checkout >/dev/null 2>&1
            printf '\n# 22ei: a closure edit so the range is adjudicated\n' \
                >> scripts/todo-graph/resolve_symbol.py
            git add scripts/todo-graph/resolve_symbol.py >/dev/null 2>&1
            git commit --quiet --no-verify -m "22ei: a head commit carrying a post-checkout hook" >/dev/null 2>&1
            git config core.hooksPath .githooks
        )
        G_S54_HOOKBASE="$( (cd "$GATE_REPO" && git rev-parse HEAD~1) )"
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_S54_HOOKBASE" --head HEAD \
            >"$TMP_DIR/gate-22ei.log" 2>&1)
        G_S54_HOOK_RC=$?
        G_S54_HOOK_COMMITTED=no
        (cd "$GATE_REPO" && git cat-file -e "HEAD:.githooks/post-checkout") >/dev/null 2>&1 \
            && G_S54_HOOK_COMMITTED=yes
        # THE PREMISE IS ASSERTED: the hook must really be in the commit, or the
        # case proves only that a file nobody added did not run.
        if [ "$G_S54_HOOK_COMMITTED" = yes ] && [ ! -e "$G_S54_HOOKMARK" ]; then
            t_pass "identity gate: a post-checkout hook in the tree under test does not run during materialization"
        else
            t_fail "identity gate: the tree under test executed code during materialization (rc=$G_S54_HOOK_RC; committed=$G_S54_HOOK_COMMITTED; marker=$([ -e "$G_S54_HOOKMARK" ] && echo present || echo absent); see $TMP_DIR/gate-22ei.log)"
        fi

        # 22ej: MUTATION, AND THE SECOND NET. Take the hook-free setting back
        # out of a copy and the SAME commit executes -- which proves 22ei
        # measures the setting rather than a hook that never fired. It also
        # reaches the independent net: the hook deletes corpus files, so the
        # materialized tree no longer matches the commit and the faithfulness
        # check must refuse rather than walk a thinned corpus. Two properties,
        # one run, because the second only exists to catch what the first misses.
        G_MUT_HOOK="$GATE_REPO/scripts/todo-graph/identity-gate-hookson.sh"
        sed 's#git -c "core.hooksPath=\$NOHOOKS_DIR" worktree add#git worktree add#g' \
            "$GATE_IN_CLONE" > "$G_MUT_HOOK"
        if cmp -s "$GATE_IN_CLONE" "$G_MUT_HOOK"; then
            t_fail "identity gate: could not build the hooks-enabled mutation (the shape moved; the mutant is identical to its source)"
        else
            rm -f "$G_S54_HOOKMARK"
            (cd "$GATE_REPO" && bash "$G_MUT_HOOK" --base "$G_S54_HOOKBASE" --head HEAD \
                >"$TMP_DIR/gate-22ej.log" 2>&1)
            G_S54_MUTHOOK_RC=$?
            if [ -e "$G_S54_HOOKMARK" ] \
               && [ "$G_S54_MUTHOOK_RC" -eq 3 ] \
               && grep -q 'MATERIALIZATION_UNFAITHFUL' "$TMP_DIR/gate-22ej.log"; then
                t_pass "identity gate: MUTATION -- hooks enabled, the tree runs AND the faithfulness check refuses it"
            else
                t_fail "identity gate: the hooks mutation proved nothing (rc=$G_S54_MUTHOOK_RC; marker=$([ -e "$G_S54_HOOKMARK" ] && echo present || echo absent); see $TMP_DIR/gate-22ej.log)"
            fi
            rm -f "$G_MUT_HOOK"
        fi
        (
            cd "$GATE_REPO" || exit 1
            git config --unset core.hooksPath >/dev/null 2>&1
            git rm --quiet -f .githooks/post-checkout >/dev/null 2>&1
            git commit --quiet --no-verify -m "22ei: drop the post-checkout hook" >/dev/null 2>&1
            git checkout --quiet HEAD -- . >/dev/null 2>&1
        )
        rm -f "$G_S54_HOOKMARK"

        # 22ek: AN EXPLICIT --head THAT CARRIES A DIFFERENT DRIVER. The refusal
        # is the same one an uncommitted edit gets, and for the same reason --
        # the gate will not certify a range with code that range does not
        # contain -- but the REPAIR is not: there is no edit to commit or stash,
        # and telling the operator to do so sends them somewhere that cannot
        # work. The two messages must differ, and the working tree here is
        # CLEAN, which is what makes the distinction load-bearing.
        G_S54_OLDHEAD="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
        (
            cd "$GATE_REPO" || exit 1
            printf '\n# 22ek: a driver edit that lives only in this commit\n' \
                >> scripts/todo-graph/identity-gate.sh
            git commit --quiet --no-verify -am "22ek: a commit whose driver differs from the checkout" >/dev/null 2>&1
            git checkout --quiet "$G_S54_OLDHEAD" -- scripts/todo-graph/identity-gate.sh >/dev/null 2>&1
            git commit --quiet --no-verify -am "22ek: and a later commit that restores it" >/dev/null 2>&1
        )
        G_S54_OTHERHEAD="$( (cd "$GATE_REPO" && git rev-parse HEAD~1) )"
        G_S54_CLEANTREE="$( (cd "$GATE_REPO" && git status --porcelain --untracked-files=all | head -3) )"
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_S54_OLDHEAD" --head "$G_S54_OTHERHEAD" \
            >"$TMP_DIR/gate-22ek.log" 2>&1)
        G_S54_OH_RC=$?
        if [ -z "$G_S54_CLEANTREE" ] && [ "$G_S54_OH_RC" -eq 3 ] \
           && grep -q 'GATE_DRIVER_NOT_AT_HEAD' "$TMP_DIR/gate-22ek.log" \
           && grep -q 'Re-run from a checkout whose' "$TMP_DIR/gate-22ek.log" \
           && ! grep -q 'Commit or stash the driver edit' "$TMP_DIR/gate-22ek.log"; then
            t_pass "identity gate: an explicit head with a different driver gets a repair it can actually perform"
        else
            t_fail "identity gate: the explicit-head driver refusal named the wrong repair (rc=$G_S54_OH_RC; dirt [$G_S54_CLEANTREE]; see $TMP_DIR/gate-22ek.log)"
        fi

        # 22el: A COMMITTED SYMLINK IS A SYMLINK WHATEVER THE FILESYSTEM CAN
        # REPRESENT. With `core.symlinks` false -- the default wherever the host
        # cannot create symlinks, which is an ordinary Windows checkout -- git
        # writes a committed 120000 entry as a small REGULAR file holding the
        # target path text and reports the tree clean. Neither the faithfulness
        # check nor `git status` can see that, because the content IS faithful;
        # only the type is not. Asking the filesystem would classify a symlinked
        # protocol file as a regular one and adjudicate the range on semantics
        # no symlink-capable checkout shares, so the head contract asks the
        # COMMIT, symmetrically with the base.
        (
            cd "$GATE_REPO" || exit 1
            git checkout --quiet HEAD -- . >/dev/null 2>&1
            cp scripts/todo-graph/snapshot_protocol.json "$TMP_DIR/s54-real-protocol.json"
            rm -f scripts/todo-graph/snapshot_protocol.json
            ln -s "$TMP_DIR/s54-real-protocol.json" scripts/todo-graph/snapshot_protocol.json
            printf '\n# 22el: a closure edit so the range is adjudicated\n' \
                >> scripts/todo-graph/resolve_symbol.py
            git add -A scripts/todo-graph >/dev/null 2>&1
            git commit --quiet --no-verify -m "22el: commit the protocol JSON as a symlink" >/dev/null 2>&1
            git config core.symlinks false
        )
        G_S54_SYMBASE="$( (cd "$GATE_REPO" && git rev-parse HEAD~1) )"
        G_S54_SYMMODE="$( (cd "$GATE_REPO" && git ls-tree HEAD -- scripts/todo-graph/snapshot_protocol.json | cut -d' ' -f1) )"
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_S54_SYMBASE" --head HEAD \
            >"$TMP_DIR/gate-22el.log" 2>&1)
        G_S54_SYM_RC=$?
        G_S54_SYM_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22el.log" | head -1)"
        # THE PREMISE IS THE COMMITTED MODE, not what happens to be on disk:
        # this case is only about a symlink the COMMIT carries.
        #
        # THE NEGATIVE IS THE LOAD-BEARING HALF, and it is unchanged: the run
        # must NEVER reach `HEAD_PROTOCOL_UNREADABLE`. Emulated, the JSON's
        # bytes are the target PATH TEXT, so the protocol would simply fail to
        # parse -- a refusal, but one that sends the operator to inspect JSON
        # when the fault is that the commit stores a symlink where code must be.
        #
        # THE POSITIVE MOVED WITH SECTION 55, AND SAYS MORE THAN IT USED TO. The
        # head protocol was read from a `git worktree add` checkout, so on this
        # host git EMULATED the symlink as a regular file and the emulation
        # probe in `tree_is_faithful` was the only thing standing between that
        # and a parse: hence `MATERIALIZATION_UNFAITHFUL`. Section 55 assembles
        # the narrow head tree from the commit's own blobs and writes a 120000
        # entry with `os.symlink`, so `core.symlinks` cannot change what the
        # gate sees at all, and the commit-side type contract answers first with
        # `HEAD_PROTOCOL_NOT_A_FILE` -- the SAME token a symlink-capable host
        # produces, which the second half of this case pins independently. The
        # assertion is therefore that emulation no longer changes the verdict,
        # which is strictly stronger than requiring it to be caught.
        # The emulation probe itself is still guarded, on the surface where it
        # still applies -- the full checkout -- by 22el-wt immediately below.
        if [ "$G_S54_SYMMODE" = "120000" ] && [ "$G_S54_SYM_RC" -eq 3 ] \
           && grep -q 'HEAD_PROTOCOL_NOT_A_FILE' "$TMP_DIR/gate-22el.log" \
           && ! grep -q 'HEAD_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22el.log"; then
            t_pass "identity gate: symlink emulation cannot change the answer, because the gate no longer reads an emulated checkout to decide it"
        else
            t_fail "identity gate: symlink emulation was misclassified or let through (rc=$G_S54_SYM_RC; mode=$G_S54_SYMMODE; got [$G_S54_SYM_TOKEN]; see $TMP_DIR/gate-22el.log)"
        fi
        # AND THE COMMIT-SIDE CONTRACT IS PINNED SEPARATELY. The check above
        # refuses BEFORE the head protocol contract is reached, so it proves the
        # emulation probe and says nothing about the other half of the fix --
        # reverting `protocol_present_in_commit` to the filesystem would leave it
        # green. With the emulation probe disabled the run reaches that contract,
        # and the commit's own recorded mode must still refuse (Codex
        # adversarial, section 54 round 3, [low]).
        G_MUT_SYM="$GATE_REPO/scripts/todo-graph/identity-gate-nosymprobe.sh"
        sed 's#^        \[ -L "\$1/\$_path" \] && continue$#        continue#' \
            "$GATE_IN_CLONE" > "$G_MUT_SYM"
        if cmp -s "$GATE_IN_CLONE" "$G_MUT_SYM"; then
            t_fail "identity gate: could not build the symlink-probe mutation (the shape moved; the mutant is identical to its source)"
        else
            (cd "$GATE_REPO" && git config core.symlinks true >/dev/null 2>&1)
            (cd "$GATE_REPO" && git checkout --quiet HEAD -- . >/dev/null 2>&1)
            (cd "$GATE_REPO" && bash "$G_MUT_SYM" --base "$G_S54_SYMBASE" --head HEAD \
                >"$TMP_DIR/gate-22el-contract.log" 2>&1)
            G_S54_CT_RC=$?
            if [ "$G_S54_CT_RC" -eq 3 ] \
               && grep -q 'HEAD_PROTOCOL_NOT_A_FILE' "$TMP_DIR/gate-22el-contract.log"; then
                t_pass "identity gate: the commit's own mode refuses a symlinked protocol file on a symlink-capable host"
            else
                t_fail "identity gate: the commit-side file-type contract did not refuse a committed symlink (rc=$G_S54_CT_RC; see $TMP_DIR/gate-22el-contract.log)"
            fi
            rm -f "$G_MUT_SYM"
        fi
        (
            cd "$GATE_REPO" || exit 1
            git config --unset core.symlinks >/dev/null 2>&1
            rm -f scripts/todo-graph/snapshot_protocol.json
            git checkout --quiet "$G_S54_SYMBASE" -- scripts/todo-graph/snapshot_protocol.json >/dev/null 2>&1
            git commit --quiet --no-verify -m "22el: restore the protocol JSON as a regular file" >/dev/null 2>&1
            git checkout --quiet HEAD -- . >/dev/null 2>&1
        )
        rm -f "$TMP_DIR/s54-real-protocol.json"

        # 22el-wt: THE EMULATION PROBE STILL GUARDS THE FULL CHECKOUT.
        #
        # Section 55 removed the two eager checkouts from the byte-identical
        # path and assembles the narrow trees from each commit's own blobs, so
        # `core.symlinks` can no longer decide what the PROTOCOL read sees --
        # which is why 22el above now asserts a commit-side token. The
        # emulation hazard did not disappear, it MOVED: a range that touches the
        # resolver closure still materializes both trees in full, and there
        # `tree_is_faithful` is the only thing that can tell a 120000 entry
        # written as a plain regular file from the real one. Left unfixtured,
        # section 54's probe would have become a check nothing exercises -- the
        # quiet way a guard stops working.
        #
        # THE SYMLINK IS ON AN UNRELATED PATH ON PURPOSE. On a protocol or
        # closure member the commit-side type contract refuses first and this
        # case would pass without the probe ever running, which is the shape
        # that makes a fixture agree with the regression it exists to catch.
        G_S55_WTBASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
        (
            cd "$GATE_REPO" || exit 1
            git config core.symlinks true >/dev/null 2>&1
            git checkout --quiet HEAD -- . >/dev/null 2>&1
            ln -sfn "TODO-Claude-Overnight-Runner.md" todo/s55-emulated-link.md
            printf '\n# 22el-wt: a benign closure edit, so the range checks out in full\n' \
                >> scripts/todo-graph/resolve_symbol.py
            git add -A todo/s55-emulated-link.md scripts/todo-graph/resolve_symbol.py >/dev/null 2>&1
            git commit --quiet --no-verify -m "22el-wt: an unrelated committed symlink beside a closure edit" >/dev/null 2>&1
            git config core.symlinks false >/dev/null 2>&1
        )
        G_S55_WTMODE="$( (cd "$GATE_REPO" && git ls-tree HEAD -- todo/s55-emulated-link.md | cut -d' ' -f1) )"
        # The emulation only happens if git REWRITES the entry, so force it:
        # remove the symlink and let a fresh checkout re-create it under the
        # setting, which is what an ordinary clone on such a host does.
        (cd "$GATE_REPO" && rm -f todo/s55-emulated-link.md \
            && git checkout --quiet HEAD -- todo/s55-emulated-link.md >/dev/null 2>&1)
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_S55_WTBASE" --head HEAD \
            >"$TMP_DIR/gate-22el-wt.log" 2>&1)
        G_S55_WT_RC=$?
        G_S55_WT_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22el-wt.log" | head -1)"
        if [ "$G_S55_WTMODE" = "120000" ] && [ "$G_S55_WT_RC" -eq 3 ] \
           && grep -q 'MATERIALIZATION_UNFAITHFUL' "$TMP_DIR/gate-22el-wt.log"; then
            t_pass "identity gate: symlink emulation in the FULL checkout is still named as an unfaithful materialization"
        else
            t_fail "identity gate: the full-checkout emulation probe did not fire (rc=$G_S55_WT_RC; mode=$G_S55_WTMODE; got [$G_S55_WT_TOKEN]; see $TMP_DIR/gate-22el-wt.log)"
        fi
        # MUTATION: without the probe the same range must NOT earn that token,
        # which is what proves the probe rather than the range is doing the work.
        G_S55_MUT="$GATE_REPO/scripts/todo-graph/identity-gate-s55nosym.sh"
        sed 's#^        \[ -L "\$1/\$_path" \] && continue$#        continue#' \
            "$GATE_IN_CLONE" > "$G_S55_MUT"
        if ! g_mutant_ok "$GATE_IN_CLONE" "$G_S55_MUT" 'MATERIALIZATION_UNFAITHFUL'; then
            t_fail "identity gate: could not build the full-checkout symlink-probe mutation (the shape moved, or the mutant is empty or unparsable)"
        else
            (cd "$GATE_REPO" && bash "$G_S55_MUT" --base "$G_S55_WTBASE" --head HEAD \
                >"$TMP_DIR/gate-22el-wt-mut.log" 2>&1)
            G_S55_MUT_RC=$?
            # THE MUTANT MUST REACH ITS ORDINARY VERDICT, not merely fail to
            # print one token. Absence alone is satisfied by a run that never
            # started, which is exactly the hole `g_mutant_ok` above closes one
            # level up; asserting the terminal rc closes the other half.
            if [ "$G_S55_MUT_RC" -eq 0 ] \
               && ! grep -q 'MATERIALIZATION_UNFAITHFUL' "$TMP_DIR/gate-22el-wt-mut.log"; then
                t_pass "identity gate: MUTATION -- with the symlink probe removed the emulated entry walks through to an ordinary verdict, so 22el-wt measures the fix"
            else
                t_fail "identity gate: the symlink-probe reversion did not reach an ordinary verdict (rc=$G_S55_MUT_RC; see $TMP_DIR/gate-22el-wt-mut.log)"
            fi
        fi
        rm -f "$G_S55_MUT"
        (
            cd "$GATE_REPO" || exit 1
            git config --unset core.symlinks >/dev/null 2>&1
            git rm --quiet -f todo/s55-emulated-link.md >/dev/null 2>&1
            git commit --quiet --no-verify -m "22el-wt: remove the emulated symlink" >/dev/null 2>&1
            git checkout --quiet HEAD -- . >/dev/null 2>&1
        )

        # 22en: THE NARROW DESTINATION IS CREATED, NEVER ADOPTED.
        #
        # The base-side protocol probe EXECUTES code from the base commit, and
        # it runs before the head side is assembled. `PYTHONPYCACHEPREFIX` is
        # exported as `$TMP_DIR/pycache`, so that code can name the gate's own
        # temporary directory -- a channel section 53 already had to close for
        # the probe's pid file. If the head destination were merely `mkdir -p`ed,
        # a symlink pre-planted at that path would be ADOPTED and every write of
        # the head assembly would land wherever it pointed, overwriting and
        # chmoding live files. `git worktree add` refused an occupied
        # destination, so this was a weakening introduced by replacing it (Codex
        # adversarial, section 55 round 1, [high]).
        #
        # THE POISON IS COMMITTED ON BOTH SIDES, because the range has to reach
        # the head assembly: a closure that differs would send the run down the
        # differential instead of the fast path this case is about. The symlink
        # TARGET is handed in by the fixture rather than derived, only so the
        # damage is observable in one place; a real one would aim at the
        # repository it was imported from.
        G_S55_CANARY="$TMP_DIR/s55-canary"
        rm -rf "$G_S55_CANARY"; mkdir -p "$G_S55_CANARY"
        printf 'untouched\n' > "$G_S55_CANARY/witness.txt"
        (
            cd "$GATE_REPO" || exit 1
            git checkout --quiet HEAD -- . >/dev/null 2>&1
            cat >> scripts/todo-graph/snapshot_protocol.py <<'S55POISON'


# 22en fixture: a base-side import that pre-plants the head destination.
import os as _os22en
try:
    _p22 = _os22en.environ.get("PYTHONPYCACHEPREFIX", "")
    _c22 = _os22en.environ.get("S55_CANARY_DIR", "")
    if _p22 and _c22:
        _d22 = _os22en.path.join(_os22en.path.dirname(_p22), "head-min")
        if not _os22en.path.lexists(_d22):
            _os22en.symlink(_c22, _d22)
except Exception:
    pass
S55POISON
            git commit --quiet --no-verify -am "22en: a protocol module that plants the head destination" >/dev/null 2>&1
        )
        G_S55_PBASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
        (
            cd "$GATE_REPO" || exit 1
            t="$(find todo -name 'TODO-*.md' | sort | head -1)"
            printf '\n- [ ] 22en: a todo-only edit, so the closure stays byte-identical\n' >>"$t"
            git commit --quiet --no-verify -am "22en: todo-only edit beside the planted destination" >/dev/null 2>&1
        )
        (cd "$GATE_REPO" && S55_CANARY_DIR="$G_S55_CANARY" bash "$GATE_IN_CLONE" \
            --base "$G_S55_PBASE" --head HEAD >"$TMP_DIR/gate-22en.log" 2>&1)
        G_S55_PL_RC=$?
        G_S55_PL_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22en.log" | head -1)"
        G_S55_PL_WITNESS="$(cat "$G_S55_CANARY/witness.txt" 2>/dev/null)"
        G_S55_PL_EXTRA="$(find "$G_S55_CANARY" -mindepth 1 | wc -l | tr -d ' ')"
        # THE REFUSAL AND THE ABSENCE OF DAMAGE ARE BOTH ASSERTED. A refusal
        # alone would pass a gate that wrote the whole head tree into the canary
        # and only then noticed, which is most of what this case is about.
        if [ "$G_S55_PL_RC" -eq 3 ] \
           && grep -q 'MATERIALIZATION_UNFAITHFUL' "$TMP_DIR/gate-22en.log" \
           && [ "$G_S55_PL_WITNESS" = "untouched" ] && [ "${G_S55_PL_EXTRA:-0}" -eq 1 ]; then
            t_pass "identity gate: a destination planted by base-side tree code is refused, and nothing is written through it"
        else
            t_fail "identity gate: a planted narrow destination was adopted (rc=$G_S55_PL_RC; got [$G_S55_PL_TOKEN]; witness='$G_S55_PL_WITNESS'; entries=$G_S55_PL_EXTRA; see $TMP_DIR/gate-22en.log)"
        fi
        # MUTATION: restore the adopting `mkdir -p` and the same range must let
        # the write through, which is what proves the check rather than the
        # fixture is doing the work.
        G_S55_PMUT="$GATE_REPO/scripts/todo-graph/identity-gate-s55adopt.sh"
        # THE REVERSION HAS TO UNDO BOTH LAYERS, and finding that out is what the
        # mutation is for. Reverting one alone left the case writing nothing,
        # because the assembler refuses an occupied destination AND opens it
        # `O_NOFOLLOW` relative to a descriptor held on TMP_DIR -- so the planted
        # symlink was still refused, one level down. Two independent checks is
        # the intended state; a mutation that removes one and finds the hole
        # still closed is evidence of that, not a broken fixture. It has to
        # remove both to show what either is worth.
        sed -e 's@    fail("something already exists at the destination %s" % dest_name)@    pass  # MUTATION: adopt whatever is there@' \
            -e 's@os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW@os.O_RDONLY | os.O_DIRECTORY@g' \
            "$GATE_IN_CLONE" > "$G_S55_PMUT"
        if ! g_mutant_ok "$GATE_IN_CLONE" "$G_S55_PMUT" 'MUTATION: adopt whatever is there'; then
            t_fail "identity gate: could not build the destination-adoption mutation (the shape moved, or the mutant is empty or unparsable)"
        else
            rm -rf "$G_S55_CANARY"; mkdir -p "$G_S55_CANARY"
            printf 'untouched\n' > "$G_S55_CANARY/witness.txt"
            (cd "$GATE_REPO" && S55_CANARY_DIR="$G_S55_CANARY" bash "$G_S55_PMUT" \
                --base "$G_S55_PBASE" --head HEAD >"$TMP_DIR/gate-22en-mut.log" 2>&1)
            G_S55_PM_EXTRA="$(find "$G_S55_CANARY" -mindepth 1 | wc -l | tr -d ' ')"
            if [ "${G_S55_PM_EXTRA:-0}" -gt 1 ]; then
                t_pass "identity gate: MUTATION -- an adopted destination is written through, so 22en measures the fix"
            else
                t_fail "identity gate: the adoption reversion wrote nothing, so 22en proves nothing (entries=$G_S55_PM_EXTRA; see $TMP_DIR/gate-22en-mut.log)"
            fi
        fi
        rm -f "$G_S55_PMUT"
        rm -rf "$G_S55_CANARY"

        # 22en-swap: AND THE NAME MAY BE SWAPPED AFTER IT IS CREATED.
        #
        # The half above plants the destination BEFORE the gate makes it, which a
        # bare `mkdir` refuses. It cannot see the other order: a descendant that
        # WAITS for the assembly to create `head-min`, renames it away and leaves
        # a symlink in its place. A name-keyed record of "this run created that
        # directory" stays true of the name while the name now points somewhere
        # else, so the fixture for one order proves nothing about the other
        # (Codex adversarial, section 55 round 2, [high]).
        #
        # THE POISON IS A DETACHED WAITER, which is the honest actor: the gate
        # reaps the probe's process group, so a child that `setsid`s itself is
        # what actually survives to race. It polls for the destination and swaps
        # it; the gate must still write nothing into the canary.
        G_S55_CANARY2="$TMP_DIR/s55-canary2"
        rm -rf "$G_S55_CANARY2"; mkdir -p "$G_S55_CANARY2"
        printf 'untouched\n' > "$G_S55_CANARY2/witness.txt"
        (
            cd "$GATE_REPO" || exit 1
            git checkout --quiet HEAD -- . >/dev/null 2>&1
            # FROM AN UNPOISONED MODULE. 22en's poison plants the destination
            # SYNCHRONOUSLY at import, so leaving it in place made the gate
            # refuse before this case's waiter had anything to swap -- the case
            # then failed for 22en's reason and proved nothing about its own.
            git checkout --quiet "$G_S55_PBASE"~1 -- scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
            cat >> scripts/todo-graph/snapshot_protocol.py <<'S55SWAP'


# 22en-swap fixture: a detached waiter that swaps the destination after it exists.
import os as _os22s, subprocess as _sp22s, sys as _sys22s
try:
    _p22s = _os22s.environ.get("PYTHONPYCACHEPREFIX", "")
    _c22s = _os22s.environ.get("S55_CANARY_DIR", "")
    _m22s = _os22s.environ.get("S55_SWAP_MARKER", "")
    if _p22s and _c22s and _m22s:
        _t22s = _os22s.path.dirname(_p22s)
        _prog = (
            "import os,time\n"
            "d=%r\nc=%r\nm=%r\n" % (_os22s.path.join(_t22s, "head-min"), _c22s, _m22s) +
            # PUBLISHED BEFORE THE LOOP, not after a successful swap. Recording
            # the identity only on success meant a waiter still POLLING had none
            # -- so it survived the fixture and could interfere with a later or
            # concurrent case, while the cleanup below believed it had signalled
            # everything (Codex adversarial, section 55, [medium]).
            "open(m, 'w').write('%d %s' % (os.getpid(), "
            "open('/proc/self/stat').read().split()[21]))\n"
            "for _ in range(20000):\n"
            "    try:\n"
            "        if os.path.isdir(d) and not os.path.islink(d):\n"
            "            os.rename(d, d + '.moved')\n"
            "            os.symlink(c, d)\n"
            "            open(m + '.swapped', 'w').write('yes')\n"
            "            break\n"
            "    except OSError:\n"
            "        pass\n"
            "    time.sleep(0.0005)\n"
        )
        _sp22s.Popen([_sys22s.executable, "-c", _prog], start_new_session=True)
except Exception:
    pass
S55SWAP
            git commit --quiet --no-verify -am "22en-swap: a protocol module that detaches a destination-swapping waiter" >/dev/null 2>&1
        )
        G_S55_SBASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
        (
            cd "$GATE_REPO" || exit 1
            t="$(find todo -name 'TODO-*.md' | sort | head -1)"
            printf '\n- [ ] 22en-swap: a todo-only edit, so the closure stays byte-identical\n' >>"$t"
            git commit --quiet --no-verify -am "22en-swap: todo-only edit beside the swapping waiter" >/dev/null 2>&1
        )
        G_S55_MARKER="$TMP_DIR/s55-swap-fired"
        rm -f "$G_S55_MARKER"
        (cd "$GATE_REPO" && S55_CANARY_DIR="$G_S55_CANARY2" S55_SWAP_MARKER="$G_S55_MARKER" \
            bash "$GATE_IN_CLONE" \
            --base "$G_S55_SBASE" --head HEAD >"$TMP_DIR/gate-22en-swap.log" 2>&1)
        G_S55_SW_RC=$?
        G_S55_SW_WITNESS="$(cat "$G_S55_CANARY2/witness.txt" 2>/dev/null)"
        G_S55_SW_EXTRA="$(find "$G_S55_CANARY2" -mindepth 1 | wc -l | tr -d ' ')"
        # THE VERDICT IS NOT PINNED, THE DAMAGE IS. Whether the waiter wins its
        # race is genuinely nondeterministic, so requiring a particular rc would
        # make this case flaky -- and the property that matters is not which
        # answer the gate gives but that the swap cannot make it write outside
        # its own directory. Every write is descriptor-relative, so the answer is
        # the same whether the waiter fired early or late.
        #
        # BUT THE WAITER MUST HAVE FIRED, or the case is vacuous: "the canary is
        # untouched" is equally true of a run where the swap never happened, and
        # a green suite would then be reporting a race it never ran (Codex
        # adversarial, section 55 round 3, [medium]). The marker is written only
        # after a rename AND a symlink both succeeded, so its presence is the
        # swap itself and not an attempt at one.
        #
        # WHAT THIS PROVES AND WHAT IT DOES NOT, said rather than implied: it
        # proves a real swap of a directory the assembly created let no byte
        # reach the canary. It does NOT prove the swap landed inside the
        # assembly's write window; the marker says rename-plus-symlink succeeded,
        # not that it succeeded BEFORE the last write (Codex adversarial, section
        # 55 round 4, [medium]). A test-controlled barrier was considered and is
        # not available: the fixture drives the TREE, and the assembler is the
        # gate's own python with nothing for a barrier to block on, so any
        # "synchronisation" would be another poll wearing a better name.
        #
        # SO THE DETERMINISTIC HALVES ARE ELSEWHERE, and this case is the
        # end-to-end sanity check that sits on top of them. 22en plants the
        # destination BEFORE creation and its mutation shows the write goes
        # through once both checks are removed; 22ep below asserts STRUCTURALLY
        # that the assembler contains no pathname-resolving write at all, which
        # is the property a timing test could only sample.
        # THE WITNESS AND THE IDENTITY ARE NOW TWO FILES. The marker is written
        # before the loop and says WHICH process to clean up; the `.swapped`
        # companion is written only after a rename AND a symlink both succeeded
        # and says the case actually exercised its race.
        if [ ! -f "$G_S55_MARKER.swapped" ]; then
            t_fail "identity gate: the 22en-swap waiter never performed a swap, so this case would have passed without testing anything (rc=$G_S55_SW_RC; see $TMP_DIR/gate-22en-swap.log)"
        elif [ "$G_S55_SW_WITNESS" = "untouched" ] && [ "${G_S55_SW_EXTRA:-0}" -eq 1 ]; then
            t_pass "identity gate: an observed post-creation swap of an assembled directory reached nothing outside the gate (rc=$G_S55_SW_RC)"
        else
            t_fail "identity gate: a post-creation destination swap redirected the assembly (rc=$G_S55_SW_RC; witness='$G_S55_SW_WITNESS'; entries=$G_S55_SW_EXTRA; see $TMP_DIR/gate-22en-swap.log)"
        fi
        # NOT `pkill -f`. A global command-line match kills every same-user
        # process that happens to contain the snippet -- including another
        # concurrent run's waiter -- which is a fixture reaching outside its own
        # run, the exact discipline 22eq was repaired into having (Codex
        # adversarial, section 55, [medium]). The waiter records its own pid and
        # start time; only that exact process is signalled.
        # READ BEFORE IT IS REMOVED, which the first version got backwards: it
        # deleted the marker and then tested for it, so the exact-PID branch
        # could never run at all.
        if [ -f "$G_S55_MARKER" ]; then
            while IFS=" " read -r _swp _swst; do
                [ -n "$_swp" ] && [ -n "$_swst" ] || continue
                _swnow="$(awk '{print $22}' "/proc/$_swp/stat" 2>/dev/null || true)"
                if [ -n "$_swnow" ] && [ "$_swnow" = "$_swst" ]; then
                    kill -KILL "$_swp" 2>/dev/null || true
                    for _swi in 1 2 3 4 5 6 7 8 9 10; do
                        kill -0 "$_swp" 2>/dev/null || break
                        sleep 0.2
                    done
                fi
            done < <(cat "$G_S55_MARKER" 2>/dev/null)
        fi
        rm -f "$G_S55_MARKER" "$G_S55_MARKER.swapped"
        rm -rf "$G_S55_CANARY2"
        (
            cd "$GATE_REPO" || exit 1
            git checkout --quiet "$G_S55_SBASE"~1 -- scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
            git commit --quiet --no-verify -m "22en-swap: restore the unpoisoned protocol module" >/dev/null 2>&1
            git checkout --quiet HEAD -- . >/dev/null 2>&1
        )

        # 22eo: THE CLOSURE CROSS-PROBE IS REAL, NOT A VALUE COMPARED WITH
        # ITSELF. `FIRST_H` is the path-EXPRESSION reading and the decision
        # loop's `$_h` is the tree-LISTING reading, so a member attributed to the
        # wrong entry must be refused. The first cut of section 55 took both from
        # the listing, which made the branch unable to fire while its comment
        # claimed a cross-probe -- a check that cannot fail is not a check (Codex
        # adversarial, section 55 round 1, [medium]).
        # The token is `CLOSURE_UNVERIFIABLE`: two probes disagreeing about one
        # immutable commit is a parser or repository fault, not a member moving,
        # and re-pointing the movement token at it would be this section
        # redefining a published token section 58 owns.
        # The mutation misattributes at the CONSUMPTION point, so the object
        # presence probe is untouched and the base side classifies normally --
        # otherwise the run would refuse for an unrelated reason and this case
        # would pass without exercising the comparison at all.
        G_S55_XBASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
        (
            cd "$GATE_REPO" || exit 1
            t="$(find todo -name 'TODO-*.md' | sort | head -1)"
            printf '\n- [ ] 22eo: a todo-only edit for the closure cross-probe\n' >>"$t"
            git commit --quiet --no-verify -am "22eo: todo-only edit, closure untouched" >/dev/null 2>&1
        )
        (cd "$GATE_REPO" && S55_CANARY_DIR="" bash "$GATE_IN_CLONE" \
            --base "$G_S55_XBASE" --head HEAD >"$TMP_DIR/gate-22eo-control.log" 2>&1)
        G_S55_X_RC=$?
        if [ "$G_S55_X_RC" -eq 0 ] \
           && grep -q 'byte-identical base..head' "$TMP_DIR/gate-22eo-control.log"; then
            t_pass "identity gate: CONTROL -- the unmutated cross-probe agrees and the range exits on the fast path"
        else
            t_fail "identity gate: the 22eo control did not reach the fast-path exit (rc=$G_S55_X_RC; see $TMP_DIR/gate-22eo-control.log)"
        fi
        G_S55_XMUT="$GATE_REPO/scripts/todo-graph/identity-gate-s55skew.sh"
        # THE DELIMITER IS NOT `#`, AND THAT COST A ROUND. The replacement text
        # first held `${#CLOSURE[@]}`, and that expansion contains BOTH `#` and
        # `@` -- each of which CLOSES the `s` command that uses it as a
        # delimiter. sed then rejected the expression, wrote nothing, and left a
        # ZERO-BYTE mutant. An empty bash script exits 0, so the case reported "a
        # misattributed closure entry rode the exit" while the gate under test
        # had never run, and `cmp -s` differs happily from an empty file, so the
        # build check every mutation here carries did not catch it. The
        # replacement now swaps adjacent indices with `^ 1`, which needs no array
        # length and so needs no delimiter-hostile expansion.
        sed 's@^        _h="\${H_OID\[\$_i\]}"$@        _h="${H_OID[$(( _i ^ 1 ))]}"@' \
            "$GATE_IN_CLONE" > "$G_S55_XMUT"
        # SO THE MUTANT IS PROVEN RUNNABLE, not merely different. Differing from
        # its source is necessary and not sufficient: a mutant that cannot parse,
        # or has no content at all, produces a verdict about nothing.
        if ! g_mutant_ok "$GATE_IN_CLONE" "$G_S55_XMUT" '_i \^ 1'; then
            t_fail "identity gate: could not build the closure-misattribution mutation (the shape moved, or the mutant is empty or unparsable)"
        else
            (cd "$GATE_REPO" && S55_CANARY_DIR="" bash "$G_S55_XMUT" \
                --base "$G_S55_XBASE" --head HEAD >"$TMP_DIR/gate-22eo-mut.log" 2>&1)
            G_S55_XM_RC=$?
            if [ "$G_S55_XM_RC" -eq 3 ] \
               && grep -q 'CLOSURE_UNVERIFIABLE: the two independent readings' "$TMP_DIR/gate-22eo-mut.log"; then
                t_pass "identity gate: MUTATION -- a listing entry attributed to the wrong member is refused by the cross-probe"
            else
                t_fail "identity gate: a misattributed closure entry rode the exit (rc=$G_S55_XM_RC; see $TMP_DIR/gate-22eo-mut.log)"
            fi
        fi
        rm -f "$G_S55_XMUT"

        # 22ep: EVERY WRITE IN THE ASSEMBLER IS DESCRIPTOR-RELATIVE, ASSERTED
        # STRUCTURALLY RATHER THAN SAMPLED.
        #
        # The swap cases above are end-to-end and their timing is
        # nondeterministic, so neither can prove the absence of a
        # pathname-resolving write -- only that one particular swap did not find
        # one. The property itself is static and can simply be read: inside
        # `materialize_subtree`s python, no `os.mkdir`, `os.open`, `os.symlink`
        # or `os.chmod` may resolve a path, and the mode must be applied through
        # a descriptor. A check on the source cannot be vacuous and cannot be
        # timing-lucky.
        G_S55_ASM="$TMP_DIR/s55-assembler.py"
        sed -n "/^import os, posixpath, subprocess, sys, pathlib$/,/^MATPY$/p" \
            "$GATE_IN_CLONE" > "$G_S55_ASM"
        # THE DETECTOR IS PYTHON, NOT `grep`, AND THAT IS NOT A STYLE CHOICE.
        # The first cut grepped line by line and reported `os.open(dest_name,
        # ...` as a name-resolving write because its `dir_fd=` sits on the NEXT
        # line -- a false positive from the detector, on the very call that
        # anchors everything else. A check that cannot read a wrapped call is not
        # reading the code. Logical lines are joined first, then judged.
        #
        # AND THE DETECTOR IS ITSELF MUTATION-TESTED, in the same process: the
        # same routine is run over a copy whose `os.fchmod` has been put back to
        # a path-resolving `os.chmod`, and it must flag that. A detector that
        # finds nothing on clean code and nothing on broken code has told you
        # nothing about either.
        G_S55_ASM_OUT="$(python3 - "$G_S55_ASM" <<'S55ASMPY'
import ast, sys, pathlib

src = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
if len(src.splitlines()) < 40 or "def dir_fd_for" not in src or "os.fchmod" not in src:
    print("EXTRACT-BAD")
    raise SystemExit(0)

# ALLOW BY LIST, OVER AN AST, and the first half of that took three attempts to
# get right.
#
# Cut one was a textual denylist of four spellings: a write through the builtin
# `open`, `pathlib.Path.write_bytes`, `os.replace` or an aliased import returned
# CLEAN. Cut two moved to an AST and added more forbidden names, which is the
# same mistake with a longer list -- `pathlib.Path(rel).open("wb").write(...)`
# still returned CLEAN, because neither `.open` nor `.write` had been thought of
# (Codex adversarial, section 55 rounds 5 and 6). A denylist can only find the
# mistakes someone predicted, which is the wrong shape for a check whose entire
# job is to catch one nobody predicted.
#
# So every call in the assembler must be NAMED HERE to pass. Adding a spelling
# is a deliberate edit to this list, which is the review this check stands in
# for. The rules on top of the list:
#   * `os.*` mutators are allowed only in their descriptor-relative form;
#   * an aliased import or any from-import is refused, so no name can be
#     reached under a spelling this list has never seen;
#   * everything else must appear in the allowed sets below, or it is reported.
#
# The AST also fixes what reading the source as TEXT could not: a `dir_fd` on a
# continuation line, and a nested call truncated by a non-greedy paren match --
# two false positives this detector produced on the very code it exists to
# clear.

OS_DIRFD_REQUIRED = {"mkdir", "open", "symlink", "link", "mknod", "rename",
                     "replace", "remove", "unlink", "rmdir", "truncate"}
# Non-mutating os helpers the assembler legitimately uses.
# `fstat` and `write` are added DELIBERATELY, which is what an allowlist is for:
# the assembler records the identity of the root it created so the parent can
# refuse a substituted one, and it does that on a descriptor the shell opened
# rather than by resolving a path (section 55). The audit flagged the first
# version of that code, which is the check working -- new code writing a path is
# exactly what it is meant to notice.
OS_ALLOWED_PLAIN = {"fsdecode", "fdopen", "fchmod", "read", "fstat", "write",
                    "close"}
# Attribute calls on locals: file objects, pipes, strings, containers. Anything
# NOT here is reported, which is what makes `Path(x).open(...)` fail.
# `run` AND `Popen` ARE NOT HERE. They are reachable only through the exact
# `subprocess.run` / `subprocess.Popen` branch, which validates the whole argv.
# Listed generically, a local alias -- `sp = subprocess; sp.run(["touch", ...])`
# -- fell through to this allowlist and never reached that validation, so the
# checker certified CLEAN over a pathname-resolving write (Codex adversarial,
# section 55 round 19, [medium], measured). The aliased-IMPORT rule did not
# help: a rebind is not an import.
ATTR_ALLOWED = {"write", "flush", "fileno", "readline", "read", "close",
                "wait", "split", "rsplit", "strip", "lstrip", "startswith",
                "decode", "encode", "join", "append", "add", "items", "get",
                "dirname", "basename"}
# `print` IS NOT HERE, AND ITS ABSENCE IS THE POINT. It was allowed because it
# is harmless-looking, and `print(content[oid], file=sys.stderr)` then wrote
# commit bytes to whatever the gate's stderr is redirected to, outside the
# anchored materialization, while the checker reported CLEAN (Codex adversarial,
# section 55 round 17, [medium], measured). The assembler reports through
# `fail()`, which writes to `sys.stderr` explicitly; it has no use for `print`,
# so allowing it bought nothing and cost a bypass.
NAME_ALLOWED = {"int", "len", "str", "bytes", "set", "list", "dict",
                "any", "all", "enumerate", "range", "isinstance", "OSError",
                "fail", "cannot_ask", "dir_fd_for", "parent_of", "SystemExit"}
ALLOWED_IMPORTS = {"os", "posixpath", "subprocess", "sys", "pathlib"}


def dotted(node):
    parts = []
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    if isinstance(node, ast.Name):
        parts.append(node.id)
        return ".".join(reversed(parts))
    return None


# EACH TRUSTED VARIABLE'S PERMITTED PROVENANCE, EXACTLY. Rejecting selected RHS
# FORMS was still a denylist: a walrus binding `(dest_name := "/tmp/x")` was not
# an Assign and slipped past the scan entirely, and `str("/tmp/x")` was an
# allowed Call rather than a Constant, so both reported clean while making
# Python ignore `dir_fd` (Codex adversarial, section 55 round 12, [medium],
# both measured). An allowlist of shapes cannot be walked around by finding a
# spelling nobody enumerated: anything that is not the ONE way each of these is
# legitimately produced is reported, whatever syntax produced it.
TRUSTED_PROVENANCE = {
    "dest_name": {"sub:sys.argv"},
    # The identity record's name comes from argv for the same reason the
    # destination's does: so the assembler resolves no name it composed itself.
    "id_name": {"sub:sys.argv"},
    "tmp_fd": {"call:int"},
    "part": {"for:call:rel.split"},
    "name": {"call:posixpath.basename"},
    "fd": {"@root_fd", "call:os.open", "sub:dir_fds"},
    "pfd": {"call:dir_fd_for"},
    # THE FILE HANDLE TOO. `fh.write` was trusted by SPELLING, so putting
    # `fh = sys.stderr` in front of the existing write made every later call --
    # write, flush, fileno, and the `fchmod` taken through `fh.fileno()` --
    # operate on inherited stderr, which is a log file outside the anchored
    # materialization, while the checker reported CLEAN (Codex adversarial,
    # section 55 round 16, [medium], measured). A name is trusted because of
    # where it came from or not at all.
    "fh": {"call:os.fdopen"},
}
TRUSTED_NAMES = set(TRUSTED_PROVENANCE)


def shape_of(node, is_for=False):
    prefix = "for:" if is_for else ""
    if node is None:
        return prefix + "<none>"
    if isinstance(node, ast.Name):
        return prefix + "@" + node.id
    if isinstance(node, ast.Call):
        return prefix + "call:" + (dotted(node.func) or "<expr>")
    if isinstance(node, ast.Subscript):
        base = node.value
        return prefix + "sub:" + (dotted(base) or "<expr>")
    if isinstance(node, ast.Attribute):
        return prefix + (dotted(node) or "<expr>")
    return prefix + "<expr>"


def offenders(text):
    try:
        tree = ast.parse(text)
    except SyntaxError as exc:
        return ["UNPARSABLE: %s" % exc]
    bad = []
    # THE TRUSTED VARIABLES' DEFINITIONS ARE PINNED, NOT JUST THEIR NAMES.
    # Accepting an operand because it is spelled `dest_name` proves nothing
    # about what `dest_name` holds: assigning it the literal "/tmp/22ep-bypass"
    # left `os.mkdir(dest_name, 0o700, dir_fd=tmp_fd)` reported clean, and an
    # absolute path makes Python IGNORE `dir_fd` -- so the anchor was removed
    # while the structural proof stayed green (Codex adversarial, section 55
    # round 11, [medium], measured). A trusted name must never be bound to a
    # literal: every legitimate one here is derived from argv, from a split, or
    # from a descriptor call.
    for node in ast.walk(tree):
        targets = []
        if isinstance(node, ast.Assign):
            targets = [(tgt, node.value, False) for tgt in node.targets]
        elif isinstance(node, (ast.AnnAssign, ast.AugAssign)):
            targets = [(node.target, node.value, False)]
        elif isinstance(node, ast.NamedExpr):
            # The walrus. Not an Assign, and the first thing that walked past
            # the previous scan.
            targets = [(node.target, node.value, False)]
        elif isinstance(node, (ast.For, ast.AsyncFor)):
            targets = [(node.target, node.iter, True)]
        elif isinstance(node, ast.withitem) and node.optional_vars is not None:
            targets = [(node.optional_vars, node.context_expr, False)]
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
            # A PARAMETER IS A BINDING. `def fail(msg, part="/tmp/x", fd=tmp_fd)`
            # rebinds two trusted names for the whole body, and every existing
            # call then performs an absolute write -- with the detector
            # reporting no offenders, because its scan covered assignments,
            # loops, with-items, walruses and comprehensions but not `ast.arg`
            # (Codex adversarial, section 55 round 18, [medium], measured). No
            # trusted name may be a parameter at all: each is produced at one
            # place in this assembler and shadowing it is never legitimate.
            spec = node.args if not isinstance(node, ast.Lambda) else node.args
            for a in (list(spec.posonlyargs) + list(spec.args) + list(spec.kwonlyargs)
                      + ([spec.vararg] if spec.vararg else [])
                      + ([spec.kwarg] if spec.kwarg else [])):
                if a.arg in TRUSTED_NAMES:
                    bad.append("%s bound as a parameter" % a.arg)
            targets = []
        elif isinstance(node, ast.comprehension):
            # A comprehension binds like a `for` and is not a `For`. Left out,
            # `[os.mkdir(part, 0o700, dir_fd=fd) for part in ['/tmp/x']]`
            # rebinds a trusted name to an absolute path -- which makes Python
            # ignore `dir_fd` -- and the detector reported it clean (Codex
            # adversarial, section 55 round 13, [medium], measured).
            targets = [(node.target, node.iter, True)]
        for tgt, val, is_for in targets:
            names = ([e for e in tgt.elts] if isinstance(tgt, (ast.Tuple, ast.List))
                     else [tgt])
            if (isinstance(tgt, (ast.Tuple, ast.List))
                    and isinstance(val, (ast.Tuple, ast.List))
                    and len(val.elts) == len(names)):
                vals = list(val.elts)
            else:
                vals = [val] * len(names)
            for nm, vl in zip(names, vals):
                if not isinstance(nm, ast.Name) or nm.id not in TRUSTED_NAMES:
                    continue
                got = shape_of(vl, is_for)
                if got not in TRUSTED_PROVENANCE[nm.id]:
                    bad.append("%s bound from %s" % (nm.id, got))
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for a in node.names:
                if a.asname or a.name not in ALLOWED_IMPORTS:
                    bad.append("import %s%s" % (a.name, " as " + a.asname if a.asname else ""))
        elif isinstance(node, ast.ImportFrom):
            bad.append("from %s import ..." % (node.module or "?"))
        elif isinstance(node, ast.Call):
            kw = {k.arg for k in node.keywords if k.arg}
            # THE VALUE, NOT MERELY THE KEYWORD. `os.mkdir(part, dir_fd=None)`
            # carries the keyword and is Python's ordinary PATHNAME-RELATIVE
            # behaviour, so a presence test reported it clean while it removed
            # the anchor the whole check exists to prove (Codex adversarial,
            # section 55 round 9, [medium]). The accepted expressions are the
            # three trusted descriptors this assembler holds, by name.
            TRUSTED_FDS = {"tmp_fd", "fd", "pfd"}
            dirfd_ok = any(
                k.arg == "dir_fd" and isinstance(k.value, ast.Name)
                and k.value.id in TRUSTED_FDS
                for k in node.keywords)
            # AND THE PATH OPERAND, because a trusted descriptor is IGNORED when
            # the path is absolute: `os.mkdir("/tmp/x", dir_fd=fd)` creates the
            # directory outside the anchored tree and satisfied every check
            # above (Codex adversarial, section 55 round 10, [medium]). The
            # assembler only ever names single components it computed itself, so
            # the operand is pinned to those variables. `os.symlink` is the one
            # whose PATH is the second argument -- its first is the link's target
            # text, which is commit content and legitimately arbitrary.
            TRUSTED_PATHS = {"part", "name", "dest_name", "id_name"}

            def path_operand_ok(call, attr):
                idx = 1 if attr == "symlink" else 0
                if len(call.args) <= idx:
                    return False
                a = call.args[idx]
                return isinstance(a, ast.Name) and a.id in TRUSTED_PATHS
            name = dotted(node.func)
            if isinstance(node.func, ast.Name):
                if node.func.id not in NAME_ALLOWED:
                    bad.append(node.func.id + "()")
            elif isinstance(node.func, ast.Attribute):
                attr = node.func.attr
                if name and name.split(".")[0] == "os" and name.count(".") == 1:
                    if attr in OS_DIRFD_REQUIRED:
                        if not dirfd_ok:
                            bad.append(name + "() without a trusted dir_fd")
                        elif not path_operand_ok(node, attr):
                            bad.append(name + "() on a path that is not a trusted component")
                    elif attr == "fchmod":
                        # `os.fchmod(1, 0o777)` re-modes whatever fd 1 points
                        # at, which is the gate's stdout and outside the
                        # materialization by construction (Codex adversarial,
                        # section 55 round 15, [medium], measured).
                        arg = node.args[0] if node.args else None
                        ok = (isinstance(arg, ast.Call)
                              and dotted(arg.func) in ("fh.fileno",))
                        if not ok:
                            bad.append("os.fchmod() on an untrusted descriptor")
                    elif attr == "fdopen":
                        # A FILE OBJECT IS ONLY AS ANCHORED AS THE DESCRIPTOR IT
                        # WRAPS. `os.fdopen` and `.write` were allowlisted
                        # independently, so `os.fdopen(1, "wb").write(b"...")`
                        # reported clean -- and fd 1 is wherever the gate's
                        # stdout happens to point, which is outside the
                        # materialization by construction (Codex adversarial,
                        # section 55 round 14, [medium], measured). The wrapped
                        # descriptor must be one this assembly opened.
                        arg = node.args[0] if node.args else None
                        if not (isinstance(arg, ast.Name) and arg.id in TRUSTED_FDS):
                            bad.append("os.fdopen() on an untrusted descriptor")
                    elif attr not in OS_ALLOWED_PLAIN:
                        bad.append(name + "()")
                elif name in ("posixpath.dirname", "posixpath.basename",
                              "posixpath.join", "sys.stderr.write"):
                    pass
                elif name in ("subprocess.run", "subprocess.Popen"):
                    # PINNED TO THE TWO READ-ONLY SHAPES, because "subprocess is
                    # allowed" is a hole the size of the shell:
                    # `subprocess.run(["touch", rel])` resolves a pathname and
                    # writes, and the previous cut of this allowlist returned
                    # CLEAN for it (Codex adversarial, section 55 round 7,
                    # [medium]). The assembler makes exactly two subprocess
                    # calls and both are git reads; anything else is reported.
                    # THE WHOLE ARGV, not its first two words. Checking a
                    # prefix allows any tail, and git's tail is where the
                    # writing subcommands live. The two shapes this assembler
                    # uses are pinned exactly: every element must be a literal
                    # except the two named variables, and a starred element is
                    # allowed only where the prefix list legitimately expands.
                    LS_TREE = ["git", "ls-tree", "-r", "-z", "@commit", "--", "*prefixes"]
                    CAT_FILE = ["git", "cat-file", "--batch"]
                    argv = node.args[0] if node.args else None
                    shape = None
                    if isinstance(argv, (ast.List, ast.Tuple)):
                        shape = []
                        for e in argv.elts:
                            if isinstance(e, ast.Constant):
                                shape.append(e.value)
                            elif isinstance(e, ast.Name):
                                shape.append("@" + e.id)
                            elif isinstance(e, ast.Starred) and isinstance(e.value, ast.Name):
                                shape.append("*" + e.value.id)
                            else:
                                shape.append("<expr>")
                    if shape not in (LS_TREE, CAT_FILE):
                        bad.append(name + "(" + repr(shape) + ")")
                elif name and name.startswith("pathlib."):
                    # pathlib is imported for PurePosixPath only; a Path(...)
                    # anywhere here is a pathname handle, which is the thing
                    # this check exists to keep out of the write paths.
                    if name != "pathlib.PurePosixPath":
                        bad.append(name + "()")
                elif attr in ("write", "flush", "fileno"):
                    # A WRITE IS ONLY ANCHORED IF ITS RECEIVER IS. `.write` was
                    # allowed on any receiver, so `sys.stdout.buffer.write(...)`
                    # reported clean while writing wherever the gate's stdout
                    # points (Codex adversarial, section 55 round 15, [medium],
                    # measured). The only handle this assembler writes through is
                    # the one it opened.
                    recv = dotted(node.func)
                    if recv not in ("fh.write", "fh.flush", "fh.fileno",
                                    "sys.stderr.write", "proc.stdin.write"):
                        bad.append("." + attr + "() on " + str(recv))
                elif attr not in ATTR_ALLOWED:
                    bad.append("." + attr + "()")
            else:
                # A CALLEE THAT IS NEITHER A NAME NOR AN ATTRIBUTE IS REPORTED,
                # not skipped. `os.__dict__["unlink"](rel)` has a Subscript
                # callee and fell straight through every branch above -- the
                # detector returned CLEAN over a real filesystem mutation, which
                # is the same fail-open it has now been repaired from three
                # times (Codex adversarial, section 55 round 8, [medium],
                # measured). Deny-by-default has to mean the DEFAULT branch
                # denies.
                bad.append("<%s callee>()" % type(node.func).__name__)
    return bad


clean = offenders(src)
# EVERY ALLOWED CATEGORY IS MUTATION-TESTED, and one of these was added because
# the detector passed it: `Path(rel).open("wb")` reaches a write through two
# calls neither of which had been named.
MUTATIONS = {
    "fchmod-to-chmod": ("os.fchmod(fh.fileno(), ", "os.chmod(dest_name + '/' + rel, "),
    "dirfd-dropped": ("os.mkdir(part, dir_fd=fd)", "os.mkdir(part)"),
    "builtin-open": ("with os.fdopen(fd, \"wb\") as fh:", "with open(rel, \"wb\") as fh:"),
    "pathlib-write": ("fh.write(content[oid])", "pathlib.Path(rel).write_bytes(content[oid])"),
    "pathlib-open": ("fh.write(content[oid])", "pathlib.Path(rel).open('wb').write(content[oid])"),
    "aliased-import": ("import os, posixpath, subprocess, sys, pathlib",
                       "import os as _o, posixpath, subprocess, sys, pathlib"),
    "subprocess-run-write": ("fh.write(content[oid])",
                             "subprocess.run(['touch', rel])"),
    "subprocess-popen-write": ("fh.write(content[oid])",
                               "subprocess.Popen(['tee', rel]).communicate()"),
    "subprocess-argv-tail": ('["git", "cat-file", "--batch"]',
                             '["git", "cat-file", "--batch", rel]'),
    "dynamic-callee": ("fh.write(content[oid])",
                       "os.__dict__['unlink'](rel)"),
    "dirfd-none": ("os.mkdir(part, dir_fd=fd)", "os.mkdir(part, dir_fd=None)"),
    "dirfd-expression": ("os.mkdir(part, dir_fd=fd)",
                         "os.mkdir(part, dir_fd=int(open('/dev/null').fileno()))"),
    "absolute-operand": ("os.mkdir(part, dir_fd=fd)",
                         "os.mkdir('/tmp/22ep-bypass', dir_fd=fd)"),
    "dotdot-operand": ("os.mkdir(part, dir_fd=fd)",
                       "os.mkdir('../22ep-bypass', dir_fd=fd)"),
    # The three that pin the VARIABLES rather than the call sites. Without
    # these the operand checks above are satisfied by a name whose value was
    # replaced somewhere else entirely.
    "dest-name-absolute": ("prefixes = sys.argv[5:]",
                           "prefixes = sys.argv[5:]\ndest_name = '/tmp/22ep-bypass'"),
    "name-dotdot": ("    name = posixpath.basename(rel)",
                    "    name = '../22ep-bypass'"),
    "part-literal": ("        cur = posixpath.join(cur, part) if cur else part",
                     "        part = '/tmp/22ep-bypass'\n        cur = posixpath.join(cur, part) if cur else part"),
    # The two that walked past a denylist of RHS forms.
    "walrus-rebind": ("os.mkdir(dest_name, 0o700, dir_fd=tmp_fd)",
                      "os.mkdir((dest_name := '/tmp/22ep-bypass'), 0o700, dir_fd=tmp_fd)"),
    "wrapped-literal": ("prefixes = sys.argv[5:]",
                        "prefixes = sys.argv[5:]\ndest_name = str('/tmp/22ep-bypass')"),
    "inherited-fd-write": ("fh.write(content[oid])",
                           'os.fdopen(1, "wb").write(b"corrupt")'),
    "inherited-fchmod": ('os.fchmod(fh.fileno(), 0o755 if mode == "100755" else 0o644)',
                         "os.fchmod(1, 0o777)"),
    "stdout-write": ("fh.write(content[oid])",
                     "sys.stdout.buffer.write(content[oid])"),
    "subprocess-alias": ("fh.write(content[oid])",
                         'sp = subprocess\n        sp.run(["touch", "/tmp/22ep-bypass"])'),
    "param-rebind": ("def fail(msg):",
                     'def fail(msg, part="/tmp/22ep-bypass", fd=tmp_fd):'),
    "print-to-stderr": ("fh.write(content[oid])",
                        "print(content[oid], file=sys.stderr)"),
    "fh-rebind": ("        fh.write(content[oid])",
                  "        fh = sys.stderr\n        fh.write(content[oid])"),
    "comprehension-rebind": ("os.mkdir(dest_name, 0o700, dir_fd=tmp_fd)",
                             "[os.mkdir(part, 0o700, dir_fd=tmp_fd) "
                             "for part in ['/tmp/22ep-comprehension-bypass']]"),
}
undetected = []
for label, (old, new) in MUTATIONS.items():
    mutant = src.replace(old, new, 1)
    if mutant == src:
        undetected.append(label + ":unchanged")
    elif not offenders(mutant):
        undetected.append(label + ":undetected")

if undetected:
    print("MUTANT-BAD " + ",".join(undetected))
elif clean:
    print("DIRTY " + "; ".join(clean)[:300])
else:
    print("CLEAN")
S55ASMPY
)"
        case "$G_S55_ASM_OUT" in
            CLEAN)
                t_pass "identity gate: no path-resolving write survives in the narrow assembler, over a deny-by-default AST audit whose every allowed category is mutation-tested" ;;
            EXTRACT-BAD)
                t_fail "identity gate: could not extract the assembler body to check it (the shape moved)" ;;
            MUTANT-*)
                t_fail "identity gate: the descriptor-relative-write detector does not detect (got [$G_S55_ASM_OUT]), so 22ep proves nothing" ;;
            *)
                t_fail "identity gate: the narrow assembler still resolves a pathname to write ($G_S55_ASM_OUT)" ;;
        esac
        rm -f "$G_S55_ASM"

        # 22er: EVERY EXTERNAL LAUNCH IS INSIDE THE DEADLINE, ENUMERATED FROM
        # THE SOURCE RATHER THAN FROM MEMORY.
        #
        # Five separate adversarial rounds each found ONE more launch outside
        # the budget -- contract_of, then the bucket checker, then the
        # relocation interpreter, then the comparison's argument substitutions,
        # then three `mktemp`s, then the reads that consume the checker's
        # output. Every one was found by a reader looking, and every fix was
        # scoped to what that reader happened to name. That is a losing shape:
        # the defect is not any particular call, it is that nothing enumerates
        # the set. This does.
        #
        # WHAT IT COVERS AND WHAT IT DOES NOT, said plainly. It reads the shell
        # body with heredoc bodies removed (their contents are python, and 22ep
        # audits those), joins line continuations, and requires every
        # command-position launch of a binary that has actually bitten -- python3,
        # git, mktemp, cat, base64, head -- to carry a `timeout` on the same
        # logical line. It does NOT prove the bound is the RIGHT one, and it does
        # not cover every binary in the file; it covers the ones whose unbounded
        # use is the recurring defect, and adding another is one word here.
        G_S55_INV="$(python3 - "$GATE_IN_CLONE" <<'S55INVPY'
import re, sys, pathlib

src = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")

# ONE TOKENIZER WITH A CONTEXT STACK, emitting every command piece it finds.
#
# This is the seventh version and the second rewrite. Version six was a single
# character pass and was still wrong in a way only a mutation could show: it
# kept the OUTER quote state while inside `$( ... )`, so the heredoc opening a
# command substitution was never recognised, and a separate paren-counter then
# walked the embedded python counting brackets it had no business reading. A
# removed bound on the relocation interpreter went undetected as a result
# (Codex adversarial, section 55 round 14, [medium], measured).
#
# The fix is structural: a command substitution is its OWN context, with its own
# quote state and its own heredocs, so the tokenizer pushes a frame at `$(` and
# pops at the matching `)`. Each frame's text is emitted as a piece in its own
# right. Comments and heredoc bodies never enter a frame at all.
#
# The history is kept in this comment deliberately. Every earlier version was
# confidently wrong in the same direction -- it reported CLEAN -- and each was
# caught only because this fixture mutation-tests itself. A structural checker
# that is not mutation-tested is a checker that reports what you hoped for.


def command_pieces(text):
    # PIECES CARRY THE LINES THEY CAME FROM. A `launch-exempt:` marker is a
    # trailing comment on a source line, and a nested `$( ... )` body is emitted
    # as its own piece WITHOUT that trailing text -- so the marker on
    # `SCRIPT_DIR="$(cd "$(dirname ...)" && pwd)"` never reached the `dirname`
    # piece it was written for. Recording each piece's line range lets the
    # exemption apply to everything that line launches, which is what a reader
    # writing the marker means by it.
    pieces, stack = [], [{"buf": "", "s": False, "d": False, "line": 1}]
    i, n = 0, len(text)
    line = 1
    pending_heredocs = []
    while i < n:
        ch = text[i]
        top = stack[-1]

        if ch == "\n":
            line += 1
            # Heredocs opened on this line consume the following lines. THE
            # SKIPPED NEWLINES ARE COUNTED: jumping the cursor past a body
            # without advancing the line number made every reported location
            # after the first heredoc point at unrelated code, which is a
            # diagnostic that actively misleads whoever reads it.
            _hd_start = i
            for tag in pending_heredocs:
                j = text.find("\n" + tag + "\n", i)
                end_j = text.find("\n", i + 1) if j == -1 else j + len(tag) + 1
                i = end_j if end_j != -1 else n
            line += text.count("\n", _hd_start, i)
            pending_heredocs = []
            if len(stack) == 1 and not top["s"] and not top["d"]:
                if top["buf"].strip():
                    pieces.append((top["buf"].strip(), top["line"], line))
                top["buf"] = ""
                top["line"] = line
            else:
                top["buf"] += " "
            i += 1
            continue

        if ch == "\\" and not top["s"]:
            # A backslash-newline is a continuation; otherwise it escapes. THE
            # LINE STILL ADVANCES: it did not, so every location after the first
            # continued command drifted earlier -- and a drifted location landed
            # on an unrelated `launch-exempt:` marker and SUPPRESSED a real
            # launch. A checker whose provenance is wrong does not merely
            # mis-report; it silently applies the wrong exemption (Codex
            # adversarial, section 55 round 15, [medium], measured).
            if i + 1 < n and text[i + 1] == "\n":
                i += 2
                line += 1
                top["buf"] += " "
                continue
            top["buf"] += text[i:i + 2]
            i += 2
            continue

        if ch == "'" and not top["d"]:
            top["s"] = not top["s"]
            top["buf"] += ch
            i += 1
            continue

        if ch == '"' and not top["s"]:
            top["d"] = not top["d"]
            top["buf"] += ch
            i += 1
            continue

        # A SUBSTITUTION IS ENTERED FROM INSIDE DOUBLE QUOTES TOO. `"$( ... )"`
        # is the commonest spelling in this file, and requiring the outer
        # context to be unquoted meant the frame was never pushed: everything
        # inside ran in the OUTER quote state, so an `awk` program's own quoting
        # flipped it and the tokenizer began emitting fragments of an awk script
        # as though they were commands. Only a SINGLE quote suppresses a
        # substitution.
        if not top["s"] and text.startswith("$(", i):
            stack.append({"buf": "", "s": False, "d": False, "line": line})
            i += 2
            continue

        if not top["s"] and not top["d"]:
            if ch == "#":
                rest_end = text.find("\n", i)
                rest_end = n if rest_end == -1 else rest_end
                if "launch-exempt:" in text[i:rest_end]:
                    top["buf"] += text[i:rest_end]
                i = rest_end
                continue
            if ch == ")" and len(stack) > 1:
                done = stack.pop()
                if done["buf"].strip():
                    pieces.append((done["buf"].strip(), done["line"], line))
                stack[-1]["buf"] += " "
                i += 1
                continue
            m = re.match(r"<<-?'?([A-Za-z0-9_]+)'?", text[i:])
            if m:
                pending_heredocs.append(m.group(1))
                top["buf"] += " "
                i += m.end()
                continue

        top["buf"] += ch
        i += 1

    for frame in stack:
        if frame["buf"].strip():
            pieces.append((frame["buf"].strip(), frame["line"], line))
    return pieces


def exempt_lines(text):
    return {i for i, l in enumerate(text.splitlines(), 1) if "launch-exempt:" in l}


def fixed_lines(text):
    return {i for i, l in enumerate(text.splitlines(), 1) if "fixed-allowance:" in l}


pieces = command_pieces(src)

# DENY BY DEFAULT OVER EXTERNAL COMMANDS, not a watchlist of six binaries. A
# watchlist only finds the launches someone predicted, and `sed` -- absent from
# it -- was reading logs the tree's own processes had just held open, reachable
# by the same rename-and-FIFO route already fixed elsewhere in this file (Codex
# adversarial, section 55 round 14, [medium]). Anything in command position that
# is not a shell builtin or a function this file defines is an external launch
# and must carry a bound, or a `launch-exempt:` marker on its line.
#
# WHAT THIS DOES NOT COVER, STATED RATHER THAN LEFT TO BE DISCOVERED. It reads
# the shapes this file actually uses: `$( ... )` substitutions (nested, and
# inside double quotes), heredocs, comments, continuations and case arms. It
# does NOT understand backtick substitution, process substitution `<( )`,
# arithmetic contexts, or the difference between a quoted and an expanding
# heredoc tag -- so a launch written in any of those would not be reported
# (Codex adversarial, section 55 round 15, [medium], each measured against a
# synthetic case).
#
# THE ACCURATE CLAIM IS ABOUT LAUNCHES, NOT SYNTAX. An earlier wording said none
# of those shapes appears in this file, which is false: arithmetic expansion is
# used in six places. What is true, and checkable, is that no EXTERNAL LAUNCH is
# written inside any of them -- the arithmetic sites compute with shell
# variables and spawn nothing. So the inventory is a real net over the real
# code, and not a proof about shell in general. Closing the remainder means a
# bash parser, which is a project rather than a patch and is filed as one.
# -> the follow-up is TODO-06 section 56, which owns this gate's spawn surface.
BUILTINS = {
    "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done",
    "case", "esac", "function", "return", "break", "continue", "in",
    "echo", "printf", "read", "local", "export", "unset", "set", "shift",
    "eval", "exec", "exit", "trap", "wait", "kill", "cd", "pwd", "test",
    "true", "false", ":", "[", "[[", "]]", "declare", "typeset", "readonly",
    "source", ".", "let", "shopt", "builtin", "command", "hash", "type",
    "getopts", "umask", "ulimit", "times", "jobs", "disown", "alias",
    "mapfile", "readarray", "pushd", "popd", "dirs",
}
# `sleep` IS NOT A BASH BUILTIN. Listing it as one was a factual error that
# silently exempted five real launches (Codex adversarial, section 55 round 15,
# [medium], measured). It is external; the grace loops that use it carry a
# marker instead, which states why a sleep cannot block rather than asserting
# something untrue about bash.
SELF_BOUNDING = ("bounded_git", "bounded_rev_parse")

FUNCS = set(re.findall(r"(?m)^([a-z_][a-z_0-9]*)\(\)\s*\{", src))
PREFIX = re.compile(r"^\s*(?:setsid|exec|nohup|command|env|time|[A-Za-z_][A-Za-z_0-9]*(?:\[[^]]*\])?=\S*)\s+")
CASE_LABEL = re.compile(r"^\s*\(?\s*[A-Za-z0-9_*?.|\[\]-]+\)\s")
# The alternation class allows WHITESPACE, because `QUOTED` has already blanked
# any quoted alternative: `0000...|"")` arrives here as `0000...| )`.
CASE_LABEL_ALT = re.compile(r"^\s*\(?\s*[A-Za-z0-9_*?.\[\]| \t-]+\)(\s|$)")
REDIR = re.compile(r"\d?>>?&?\s*\d*|<&?\s*\d*")
QUOTED = re.compile(r"'[^']*'|\"[^\"]*\"")


def unbounded_launches(all_pieces, exempt=frozenset(), fixed_ok=frozenset()):
    found = []
    for piece, lo, hi in all_pieces:
        if "launch-exempt:" in piece or any(l in exempt for l in range(lo, hi + 1)):
            continue
        # THE FIXED-ALLOWANCE MARKER NEEDS LINE PROVENANCE TOO, for the same
        # reason `launch-exempt:` does: a `$( ... )` body is emitted without the
        # trailing comment on the line that contains it.
        allow_fixed = any(l in fixed_ok for l in range(lo, hi + 1))
        code = QUOTED.sub(" ", piece)
        # THE BOUND IS CHECKED PER SEGMENT, NOT PER PIECE. A whole-piece test
        # meant `timeout 1 true; sort` was clean because the piece contained the
        # word `timeout` somewhere -- so a second command sharing the line was
        # never examined (Codex adversarial, section 55 round 15, [medium],
        # measured).
        # SELF-BOUNDING IS A PROPERTY OF A SEGMENT, NOT OF A LINE. Tested over
        # the whole piece, `bounded_git ... | cut ...` was skipped entirely
        # because `bounded_git` appeared somewhere in it -- so a real, unbounded
        # `cut` in the same pipeline was never examined (Codex adversarial,
        # section 55 round 16, [medium], measured). It is now recognised only
        # when it is that segment's own command head, which is the only position
        # in which it bounds anything.
        # REDIRECTIONS ARE NOT COMMANDS. `printf ... >&2` splits on the `&` and
        # leaves a bare `2`, which matched the command-name shape and was
        # reported as an unbounded launch -- the scanner inventing a binary
        # called 2. Removed before anything is split.
        code = REDIR.sub(" ", code)
        # AND A CASE ARM IS STRIPPED WHOLE, alternations included. Splitting on
        # `|` first tore `0|1)` apart and left `0` looking like a command; the
        # arms are separated on `;;` and each label removed before the operator
        # split runs.
        segments = []
        for arm in code.split(";;"):
            segments.extend(re.split(r"[|;&]|\bthen\b|\bdo\b|\belse\b",
                                     CASE_LABEL_ALT.sub(" ", arm, count=1)))
        for segment in segments:
            seg = segment
            while True:
                seg2 = CASE_LABEL.sub(" ", seg, count=1)
                seg2 = PREFIX.sub("", seg2, count=1)
                if seg2 == seg:
                    break
                seg = seg2
            words = seg.strip().split()
            if not words:
                continue
            if words[0] == "timeout" or "timeout" in words[:3]:
                # A BOUND IS NOT AUTOMATICALLY THE GLOBAL DEADLINE. Accepting any
                # `timeout`-headed segment let a FIXED allowance pass as though
                # it were derived from the remaining budget, which is how a
                # cleanup helper came to launch work ten seconds past the
                # ceiling (Codex adversarial, section 55 round 18, [medium]). A
                # literal allowance is legitimate in a few places -- the clock
                # probe cannot measure itself, and exit cleanup runs after the
                # budget is gone by definition -- so it is allowed only where
                # the line says so.
                # `-s KILL` takes a VALUE, and skipping only dash-words read
                # `KILL` as the allowance -- so every fixed bound in the file
                # passed the check that was written to find them.
                allowance = ""
                for w_i, w in enumerate(words):
                    if w == "timeout":
                        rest = words[w_i + 1:]
                        k = 0
                        while k < len(rest):
                            w2 = rest[k]
                            if w2 in ("-s", "--signal", "-k", "--kill-after"):
                                k += 2
                                continue
                            if w2.startswith("-"):
                                k += 1
                                continue
                            allowance = w2
                            break
                        break
                if allowance.isdigit() and not allow_fixed and "fixed-allowance:" not in piece:
                    found.append("line %d: fixed allowance %ss -- %s"
                                 % (lo, allowance, piece.strip()[:80]))
                    break
                continue
            head_word = words[0]
            if head_word in SELF_BOUNDING:
                continue
            if (head_word in BUILTINS or head_word in FUNCS
                    or "=" in head_word or head_word.startswith("$")
                    or head_word.startswith("-") or head_word.startswith("{")
                    or head_word.startswith("}") or head_word.startswith("!")
                    or not re.match(r"^[A-Za-z0-9_./-]+$", head_word)):
                continue
            found.append("line %d: %s" % (lo, piece.strip()[:100]))
            break
    return found


bad = unbounded_launches(pieces, exempt_lines(src), fixed_lines(src))

# EVERY BOUND THIS SECTION ADDED IS MUTATION-TESTED, including the one whose
# heredoc broke the previous tokenizer.
# The separator is `[\s\\]+`, not `\s+`: these launches wrap with a
# backslash-newline, and a backslash is not whitespace -- so three of the five
# patterns matched nothing and reported "unchanged", which is the mutation
# harness correctly refusing to certify a mutation it never made.
_SEP = r"[\s\\]+"
MUTATIONS = {
    "assembler": r'timeout -s KILL "\$BUDGET_LEFT"' + _SEP + r'(?=python3 - "\$_commit")',
    "checker-output-cat": r'timeout --foreground -s KILL "\$PHASE_BUDGET"' + _SEP + r'(?=cat "\$\(tmp_path bucket-contract\.out\)")',
    "protocol-record-head": r'timeout --foreground -s KILL "\$PROBE_BUDGET"' + _SEP + r'(?=head -n 1 "\$_rec")',
    "relocation-interpreter": r'timeout --foreground -s KILL "\$PHASE_BUDGET"' + _SEP + r'(?=python3 - "\$BASE_PRE_B64")',
    "walk-log-sed": r'timeout --foreground -s KILL "\$PHASE_BUDGET"' + _SEP + r'(?=sed \047s/\^/    /\047 "\$\(tmp_path base-walk\.log\)")',
    # The filesystem helper that replaced a false exemption. Removing its bound
    # must be caught, or the helper is only the exemption in a longer form.
    "bounded-fs": r'timeout --foreground -s KILL "\$BUDGET_LEFT"' + _SEP + r'(?="\$@" 2>/dev/null)',
}
missed = []
for label, pattern in MUTATIONS.items():
    mutated = re.sub(pattern, "", src, count=1)
    if mutated == src:
        missed.append(label + ":unchanged")
    elif not unbounded_launches(command_pieces(mutated), exempt_lines(mutated), fixed_lines(mutated)):
        missed.append(label + ":undetected")

if missed:
    print("MUTANT-BAD " + ",".join(missed))
elif bad:
    print("UNBOUNDED " + " || ".join(bad[:6]))
else:
    print("CLEAN")
S55INVPY
)"
        case "$G_S55_INV" in
            CLEAN) t_pass "identity gate: every watched external launch in the shell body carries a timeout, enumerated from the source and proven by removing one" ;;
            MUTANT-*) t_fail "identity gate: the launch inventory does not detect an unbounded launch ($G_S55_INV), so 22er proves nothing" ;;
            *)     t_fail "identity gate: an external launch runs outside the deadline ($G_S55_INV)" ;;
        esac

        # 22eq: A STALLED `git` UNDER THE ASSEMBLER DOES NOT OUTLIVE THE GATE.
        #
        # The assembler runs two git processes of its own. It was bounded with
        # `timeout --foreground`, which documents that it does not time out
        # CHILDREN of the command -- so a KILL delivered to python while it was
        # blocked reading a stalled `cat-file --batch` orphaned that git, past
        # the deadline this file advertises (Codex adversarial, section 55 round
        # 4, [medium]). Whose child it is has no bearing on whether it outlives
        # the bound, which is why the assembler now runs supervised in its own
        # session like every other phase.
        # THE ORACLE IS FIXTURE-OWNED, not a global process search. The first
        # cut accepted "rc 3 and no `sleep 120` anywhere on this host", which is
        # wrong in both directions: an unrelated early refusal satisfies it
        # without the stall ever being reached, and an unrelated same-user sleep
        # fails it and is then killed by a global `pkill` -- a fixture reaching
        # outside its own run, which is the defect section 53's fixture reaper
        # was repaired for (Codex adversarial, section 55 round 5, [medium]).
        # The shim records that it was REACHED and the exact pid it became, so
        # the case asserts about that process and cleans up only that one.
        G_S55_STALLDIR="$TMP_DIR/gitshim-s55stall"
        G_S55_STALLPID="$TMP_DIR/s55-stall.pid"
        rm -f "$G_S55_STALLPID"
        mkdir -p "$G_S55_STALLDIR"
        # PUBLISHED AFTER THE CHILD EXISTS, NEVER BEFORE THE EXEC. Writing the
        # marker and then `exec`ing meant a FAILED exec still left the marker: the
        # gate would refuse for an unrelated reason, no process would survive, and
        # the case would report a reaped stall that never existed. The helper now
        # starts the blocking child FIRST and publishes only once it has confirmed
        # it running, so the marker is evidence rather than intent.
        #
        # AND THE IDENTITY IS PID PLUS START TIME, not pid plus a substring. A pid
        # is reused; `/proc/<pid>/stat` field 22 is the process's start time in
        # clock ticks, so the pair identifies THIS process and cannot match a
        # stranger that inherited its number (Codex adversarial, section 55 round
        # 6, [medium]).
        {
            printf '#!/usr/bin/env bash\n'
            printf '_cf=0; _b=0\n'
            printf 'for a in "$@"; do [ "$a" = "cat-file" ] && _cf=1; [ "$a" = "--batch" ] && _b=1; done\n'
            printf 'if [ "$_cf" = "1" ] && [ "$_b" = "1" ]; then\n'
            printf '  sleep 120 &\n'
            printf '  _sp=$!\n'
            printf '  if kill -0 "$_sp" 2>/dev/null; then\n'
            printf '    _st="$(awk "{print \\$22}" /proc/$_sp/stat 2>/dev/null)"\n'
            printf '    [ -n "$_st" ] && printf "%%s %%s\\n" "$_sp" "$_st" >> %s\n' "$G_S55_STALLPID"
            printf '  fi\n'
            printf '  wait "$_sp"\n'
            printf '  exit 0\n'
            printf 'fi\n'
            printf 'exec %s "$@"\n' "$G_REAL_GIT"
        } > "$G_S55_STALLDIR/git"
        chmod +x "$G_S55_STALLDIR/git"
        G_S55_QBASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
        (
            cd "$GATE_REPO" || exit 1
            t="$(find todo -name 'TODO-*.md' | sort | head -1)"
            printf '\n- [ ] 22eq: a todo-only edit for the stalled-assembler case\n' >>"$t"
            git commit --quiet --no-verify -am "22eq: todo-only edit" >/dev/null 2>&1
        )
        (cd "$GATE_REPO" && PATH="$G_S55_STALLDIR:$PATH" IDENTITY_GATE_BUDGET_SECS=8 \
            bash "$GATE_IN_CLONE" --base "$G_S55_QBASE" --head HEAD \
            >"$TMP_DIR/gate-22eq.log" 2>&1)
        G_S55_Q_RC=$?
        # THE STALL MUST HAVE BEEN REACHED, or the case proves nothing: an
        # unrelated rc 3 is not evidence that a child was ever created, let alone
        # reaped.
        G_S55_Q_HITS="$(grep -c . "$G_S55_STALLPID" 2>/dev/null || echo 0)"
        G_S55_Q_LEFT=0
        while IFS=" " read -r _qp _qst; do
            [ -n "$_qp" ] && [ -n "$_qst" ] || continue
            # IDENTIFIED BY START TIME, so a reissued pid is neither counted nor
            # signalled. An unreadable stat is treated as GONE rather than as a
            # survivor: the fixture must not fail on a process it cannot see, and
            # it must never signal one it cannot identify.
            _qnow="$(awk '{print $22}' "/proc/$_qp/stat" 2>/dev/null || true)"
            if [ -n "$_qnow" ] && [ "$_qnow" = "$_qst" ]; then
                G_S55_Q_LEFT=$((G_S55_Q_LEFT + 1))
                kill -KILL "$_qp" 2>/dev/null || true
            fi
        done < <(cat "$G_S55_STALLPID" 2>/dev/null)
        if [ "${G_S55_Q_HITS:-0}" -lt 1 ]; then
            t_fail "identity gate: the 22eq stall was never reached, so the case would have passed without testing anything (rc=$G_S55_Q_RC; see $TMP_DIR/gate-22eq.log)"
        elif [ "$G_S55_Q_RC" -eq 3 ] && [ "$G_S55_Q_LEFT" -eq 0 ]; then
            t_pass "identity gate: a stalled git under the narrow assembler is reaped with it, not orphaned past the deadline"
        else
            t_fail "identity gate: a stalled assembler child outlived the gate (rc=$G_S55_Q_RC; reached=$G_S55_Q_HITS survivors=$G_S55_Q_LEFT; see $TMP_DIR/gate-22eq.log)"
        fi
        rm -f "$G_S55_STALLPID"
        rm -rf "$G_S55_STALLDIR"

        (
            cd "$GATE_REPO" || exit 1
            git checkout --quiet "$G_S55_PBASE"~1 -- scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
            git commit --quiet --no-verify -m "22en: restore the unpoisoned protocol module" >/dev/null 2>&1
            git checkout --quiet HEAD -- . >/dev/null 2>&1
        )

        # 22es: A FAILED MODE PROBE IS NOT A PASSING MODE.
        #
        # The gate refuses a head that stores `identity-gate.sh` non-executable,
        # because the pre-push hook enters its identity-gate block only when that
        # file is executable -- so such a head has the local gate silently
        # disabled for every later push, with only CI still adjudicating, after
        # `main` has already moved. A refactor that removed a `cut` from that
        # line also discarded `bounded_git`'s status, and `bounded_git` answers 2
        # for "could not ask": ONE failed probe left the mode empty, the refusal
        # was skipped, and the range reached the byte-identical exit (Codex
        # adversarial, section 55, [high]).
        #
        # The shim fails ONLY that probe. It is distinguishable from the batched
        # closure listing by shape: this one names the driver path and does not
        # pass `-z`, while the batch passes `-z` and names every member.
        G_S55_MODEDIR="$TMP_DIR/gitshim-s55mode"
        mkdir -p "$G_S55_MODEDIR"
        {
            printf '#!/usr/bin/env bash\n'
            printf '_lt=0; _z=0; _drv=0\n'
            printf 'for a in "$@"; do\n'
            printf '  [ "$a" = "ls-tree" ] && _lt=1\n'
            printf '  [ "$a" = "-z" ] && _z=1\n'
            printf '  [ "$a" = "scripts/todo-graph/identity-gate.sh" ] && _drv=1\n'
            printf 'done\n'
            printf 'if [ "$_lt" = "1" ] && [ "$_z" = "0" ] && [ "$_drv" = "1" ]; then exit 128; fi\n'
            printf 'exec %s "$@"\n' "$G_REAL_GIT"
        } > "$G_S55_MODEDIR/git"
        chmod +x "$G_S55_MODEDIR/git"
        G_S55_MBASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
        (
            cd "$GATE_REPO" || exit 1
            t="$(find todo -name 'TODO-*.md' | sort | head -1)"
            printf '\n- [ ] 22es: a todo-only edit for the mode-probe case\n' >>"$t"
            git commit --quiet --no-verify -am "22es: todo-only edit" >/dev/null 2>&1
        )
        (cd "$GATE_REPO" && PATH="$G_S55_MODEDIR:$PATH" bash "$GATE_IN_CLONE" \
            --base "$G_S55_MBASE" --head HEAD >"$TMP_DIR/gate-22es.log" 2>&1)
        G_S55_M_RC=$?
        if [ "$G_S55_M_RC" -eq 3 ] \
           && ! grep -q 'byte-identical base..head' "$TMP_DIR/gate-22es.log"; then
            t_pass "identity gate: an unreadable driver-mode probe refuses instead of skipping the executable-bit invariant"
        else
            t_fail "identity gate: a failed mode probe let the range through (rc=$G_S55_M_RC; see $TMP_DIR/gate-22es.log)"
        fi
        # MUTATION: discard the probe's status again, and the same shim must let
        # the range reach the byte-identical exit -- which is what proves this
        # case measures the check and not the shim.
        # THE MUTATION TOUCHES ONE BRANCH, and the first version did not. A bare
        # `s@^case "$?" in@case 0 in@` hit all FIVE occurrences in the file,
        # including the replacement-substitution check whose ordinary rc 1 then
        # became `case 0` -- so the mutant refused with BASE_HISTORY_INCOMPLETE
        # before it ever reached the mode probe, and the assertion accepted that
        # unrelated rc 3 as proof (Codex adversarial, section 55, [medium]). A
        # mutation that changes five things proves nothing about any of them.
        # Anchored on the assignment and applied to the NEXT line only, with the
        # replacement count asserted.
        G_S55_MMUT="$GATE_REPO/scripts/todo-graph/identity-gate-s55mode.sh"
        # AND IT REMOVES BOTH GUARDS, because there are two. The status check is
        # backed by an emptiness check, so discarding the status alone still
        # refuses -- correctly, and the stricter assertion below is what
        # revealed it. Two independent guards is the intended state; a mutation
        # that removes one and finds the hole still closed is evidence of that,
        # not a broken fixture. It has to remove both to show what either is
        # worth, which is the same lesson the destination-adoption mutation
        # taught in this section.
        sed -e '/_GATE_MODE_ENTRY="\$(bounded_git ls-tree/{n;s@^case "\$?" in@case 0 in@;}' \
            -e 's@^\[ -n "\$_GATE_MODE_ENTRY" \] \\$@[ 1 ] \\@' \
            "$GATE_IN_CLONE" > "$G_S55_MMUT"
        G_S55_MM_COUNT="$(grep -c '^case 0 in$' "$G_S55_MMUT" 2>/dev/null || echo 0)"
        G_S55_MM_COUNT2="$(grep -c '^\[ 1 \] \\$' "$G_S55_MMUT" 2>/dev/null || echo 0)"
        if ! g_mutant_ok "$GATE_IN_CLONE" "$G_S55_MMUT" '^case 0 in$' \
           || [ "${G_S55_MM_COUNT:-0}" -ne 1 ] || [ "${G_S55_MM_COUNT2:-0}" -ne 1 ]; then
            t_fail "identity gate: could not build the discarded-status mutation targeting exactly one branch (status=$G_S55_MM_COUNT emptiness=$G_S55_MM_COUNT2; the shape moved)"
        else
            (cd "$GATE_REPO" && PATH="$G_S55_MODEDIR:$PATH" bash "$G_S55_MMUT" \
                --base "$G_S55_MBASE" --head HEAD >"$TMP_DIR/gate-22es-mut.log" 2>&1)
            G_S55_MM_RC=$?
            # THE MUTANT MUST REACH THE EXIT THE REAL GATE REFUSES. Accepting
            # "any rc other than 3" is what let an unrelated refusal pass as
            # proof; the fixed gate refuses this range, so the mutant has to
            # sail past it to the byte-identical exit and say so.
            if [ "$G_S55_MM_RC" -eq 0 ] \
               && grep -q 'byte-identical base..head' "$TMP_DIR/gate-22es-mut.log"; then
                t_pass "identity gate: MUTATION -- discarding the probe status lets the range reach the byte-identical exit, so 22es measures the check"
            else
                t_fail "identity gate: the discarded-status mutation did not reach the byte-identical exit (rc=$G_S55_MM_RC; see $TMP_DIR/gate-22es-mut.log)"
            fi
        fi
        rm -f "$G_S55_MMUT"
        rm -rf "$G_S55_MODEDIR"

        # 22et: THE ASSEMBLED TREE IS RE-BOUND AFTER CODE RUNS FROM IT.
        #
        # `proto_of` executes modules out of the narrow head tree, so between
        # that probe and everything downstream of it there is a window in which
        # the tree can change. Section 55 removed the `hash-object` over the
        # materialized tree when it took the closure reading from the commit --
        # and that hash was the only thing tying the bytes the gate EXECUTES to
        # the commit it names. Both readings then agreed by construction and
        # noticed nothing about the directory in between (Codex adversarial,
        # section 55, [high]).
        #
        # The poison is the protocol module itself, which is what `proto_of`
        # imports: at import it locates its own directory and rewrites a SIBLING
        # closure member there. Nothing else in the run would see that.
        G_S55_RBASE_PRE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
        (
            cd "$GATE_REPO" || exit 1
            git checkout --quiet HEAD -- . >/dev/null 2>&1
            cat >> scripts/todo-graph/snapshot_protocol.py <<'S55REBIND'


# 22et fixture: at import, rewrite a sibling closure member in whatever
# directory this module was loaded from.
import os as _os22t
try:
    _d22t = _os22t.path.dirname(_os22t.path.abspath(__file__))
    with open(_os22t.path.join(_d22t, "resolve_symbol.py"), "a") as _f22t:
        _f22t.write("\n# 22et: rewritten after the gate read me\n")
except Exception:
    pass
S55REBIND
            git commit --quiet --no-verify -am "22et: a protocol module that rewrites a sibling after being read" >/dev/null 2>&1
        )
        G_S55_RBASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
        (
            cd "$GATE_REPO" || exit 1
            t="$(find todo -name 'TODO-*.md' | sort | head -1)"
            printf '\n- [ ] 22et: a todo-only edit, so the closure stays byte-identical\n' >>"$t"
            git commit --quiet --no-verify -am "22et: todo-only edit beside the rewriting module" >/dev/null 2>&1
        )
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_S55_RBASE" --head HEAD \
            >"$TMP_DIR/gate-22et.log" 2>&1)
        G_S55_RB_RC=$?
        # ASSERTED ON THE TOKEN, NOT ON PROSE. The first version grepped a
        # sentence fragment and broke the moment the check was rewritten to say
        # the same thing differently -- a fixture pinned to wording fails for
        # reasons that have nothing to do with behaviour.
        if [ "$G_S55_RB_RC" -eq 3 ] \
           && grep -q 'MATERIALIZATION_UNFAITHFUL' "$TMP_DIR/gate-22et.log" \
           && ! grep -q 'byte-identical base..head' "$TMP_DIR/gate-22et.log"; then
            t_pass "identity gate: a closure member rewritten by the tree's own protocol module is caught before anything consumes the read"
        else
            t_fail "identity gate: a tree that changed under the gate reached its verdict (rc=$G_S55_RB_RC; see $TMP_DIR/gate-22et.log)"
        fi
        # MUTATION: remove the re-binding and the same range must reach the exit,
        # which is what proves the check rather than the poison is doing the work.
        G_S55_RBMUT="$GATE_REPO/scripts/todo-graph/identity-gate-s55norebind.sh"
        sed 's@^min_tree_still_matches @: min_tree_still_matches @' \
            "$GATE_IN_CLONE" > "$G_S55_RBMUT"
        if ! g_mutant_ok "$GATE_IN_CLONE" "$G_S55_RBMUT" '^: min_tree_still_matches '; then
            t_fail "identity gate: could not build the no-rebinding mutation (the shape moved)"
        else
            (cd "$GATE_REPO" && bash "$G_S55_RBMUT" --base "$G_S55_RBASE" --head HEAD \
                >"$TMP_DIR/gate-22et-mut.log" 2>&1)
            G_S55_RBM_RC=$?
            if [ "$G_S55_RBM_RC" -eq 0 ] \
               && grep -q 'byte-identical base..head' "$TMP_DIR/gate-22et-mut.log"; then
                t_pass "identity gate: MUTATION -- without the re-binding the rewritten tree rides the exit, so 22et measures the check"
            else
                t_fail "identity gate: the no-rebinding mutation did not reach the exit, so 22et proves nothing (rc=$G_S55_RBM_RC; see $TMP_DIR/gate-22et-mut.log)"
            fi
        fi
        rm -f "$G_S55_RBMUT"
        (
            cd "$GATE_REPO" || exit 1
            git checkout --quiet "$G_S55_RBASE_PRE" -- scripts/todo-graph/snapshot_protocol.py scripts/todo-graph/resolve_symbol.py >/dev/null 2>&1
            git commit --quiet --no-verify -m "22et: restore the unpoisoned modules" >/dev/null 2>&1
            git checkout --quiet HEAD -- . >/dev/null 2>&1
        )

        # 22em: A REGISTRATION OUTLIVES ITS DIRECTORY, so cleanup is driven by
        # what the run ATTEMPTED and not by what is still on disk. The failure
        # this pins is silent by construction: the gate exits with a perfectly
        # ordinary verdict while leaving an administrative entry in
        # `.git/worktrees` that nothing will ever collect, and it accumulates one
        # per run. Forced with a shim that deletes the linked directory the
        # moment it has been created, which is the shape an interrupted `add`
        # and a head-side module deleting its own tree both reduce to.
        G_S54_WTBASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
        (
            cd "$GATE_REPO" || exit 1
            # A CLOSURE-TOUCHING RANGE, BECAUSE THAT IS THE ONE THAT CHECKS
            # OUT (section 55). This was a todo-only edit, which now takes the
            # byte-identical fast path and materializes no linked tree at all --
            # so the shim never fired, `adds` stayed 0, and the case measured
            # nothing while reporting a failure. The registration hygiene this
            # pins is unrelated to WHY a tree is created, so the cheapest honest
            # repair is to give it a range that creates one. The edit is a
            # comment, so the resolver differential still agrees and the run
            # reaches its ordinary verdict, which is what this case needs.
            printf '\n# 22em: a benign closure edit, so the range materializes a linked tree\n' \
                >> scripts/todo-graph/resolve_symbol.py
            git commit --quiet --no-verify -am "22em: benign closure change beside a vanishing linked tree" >/dev/null 2>&1
        )
        G_S54_WTBEFORE="$( (cd "$GATE_REPO" && git worktree list --porcelain | grep -c '^worktree ') )"
        G_SHIM_S54="$TMP_DIR/gitshim-s54"
        mkdir -p "$G_SHIM_S54"
        {
            printf '#!/usr/bin/env bash\n'
            printf '_is_add=0\n'
            printf 'for a in "$@"; do [ "$a" = "add" ] && _is_add=1; done\n'
            printf '_is_wt=0\n'
            printf 'for a in "$@"; do [ "$a" = "worktree" ] && _is_wt=1; done\n'
            printf 'if [ "$_is_wt" = "1" ] && [ "$_is_add" = "1" ]; then\n'
            printf '  %s "$@"; _rc=$?\n' "$G_REAL_GIT"
            printf '  _last=""; for a in "$@"; do case "$a" in */base|*/head) _last="$a" ;; esac; done\n'
            printf '  if [ -n "$_last" ]; then printf "%%s\\n" "$_last" >> %s; rm -rf "$_last"; fi\n' \
                "$TMP_DIR/s54-adds-seen"
            printf '  exit $_rc\n'
            printf 'fi\n'
            printf 'exec %s "$@"\n' "$G_REAL_GIT"
        } > "$G_SHIM_S54/git"
        chmod +x "$G_SHIM_S54/git"
        (cd "$GATE_REPO" && PATH="$G_SHIM_S54:$PATH" bash "$GATE_IN_CLONE" \
            --base "$G_S54_WTBASE" --head HEAD >"$TMP_DIR/gate-22em.log" 2>&1)
        G_S54_WT_RC=$?
        G_S54_WTAFTER="$( (cd "$GATE_REPO" && git worktree list --porcelain | grep -c '^worktree ') )"
        # THE SHIM MUST HAVE FIRED, AND THE REGISTRATION MUST BE GONE BY NAME.
        # A count-only oracle passes when the gate exits before materializing
        # anything at all, and passes again when the gate leaks its own entry
        # while the repository-wide prune happens to collect an unrelated one --
        # so it agrees with two of the regressions it exists to catch (Codex
        # adversarial, section 54 round 3, [medium]).
        G_S54_ADDS="$(grep -c . "$TMP_DIR/s54-adds-seen" 2>/dev/null || echo 0)"
        G_S54_STILL="$( (cd "$GATE_REPO" && git worktree list --porcelain 2>/dev/null \
            | grep -c '/base$\|/head$') || true )"
        if [ "${G_S54_ADDS:-0}" -ge 1 ] && [ "${G_S54_STILL:-0}" -eq 0 ] \
           && [ "$G_S54_WTAFTER" = "$G_S54_WTBEFORE" ]; then
            t_pass "identity gate: a linked tree that vanishes under the gate leaves no registration behind"
        else
            t_fail "identity gate: a vanished linked tree stayed registered (rc=$G_S54_WT_RC; adds=$G_S54_ADDS still=$G_S54_STILL; worktrees $G_S54_WTBEFORE -> $G_S54_WTAFTER; see $TMP_DIR/gate-22em.log)"
        fi
        # MUTATION: put the directory-existence guard back in place of the
        # attempt list, and the same shim must leave the registration behind.
        # Without it the case cannot tell "cleanup handled it" from "there was
        # never anything to handle".
        G_MUT_WT="$GATE_REPO/scripts/todo-graph/identity-gate-dirguard.sh"
        sed 's#^        \[ "\$_wt_seen" -eq 1 \] || continue$#        [ -d "$_wt" ] || continue#' \
            "$GATE_IN_CLONE" > "$G_MUT_WT"
        # THE MUTANT MUST DIFFER FROM ITS SOURCE, asserted rather than inferred
        # from the absence of the old text. The previous guard checked that the
        # pre-mutation shape was gone -- which it was, because the shape had
        # been REPLACED for an unrelated reason -- so the sed matched nothing,
        # the case ran the unmutated gate, and it reported that the fix it was
        # supposed to contradict did not leak. A mutation fixture that cannot
        # tell "applied and had no effect" from "never applied" proves nothing
        # in either direction.
        if cmp -s "$GATE_IN_CLONE" "$G_MUT_WT"; then
            t_fail "identity gate: could not build the cleanup-reversion mutation (the shape moved; the mutant is identical to its source)"
        else
            rm -f "$TMP_DIR/s54-adds-seen"
            (cd "$GATE_REPO" && PATH="$G_SHIM_S54:$PATH" bash "$G_MUT_WT" \
                --base "$G_S54_WTBASE" --head HEAD >"$TMP_DIR/gate-22em-mut.log" 2>&1)
            G_S54_MUTSTILL="$( (cd "$GATE_REPO" && git worktree list --porcelain 2>/dev/null \
                | grep -c '/base$\|/head$') || true )"
            if [ "${G_S54_MUTSTILL:-0}" -ge 1 ]; then
                t_pass "identity gate: MUTATION -- the directory guard leaks the registration, so 22em measures the fix"
            else
                t_fail "identity gate: the cleanup reversion did not leak, so 22em proves nothing (still=$G_S54_MUTSTILL; see $TMP_DIR/gate-22em-mut.log)"
            fi
            # The mutation leaked on purpose; collect it so later cases start
            # from the registry they expect.
            (cd "$GATE_REPO" && git worktree prune --expire now >/dev/null 2>&1 || true)
            rm -f "$G_MUT_WT"
        fi
        # AND AN UNANSWERABLE LISTING MUST NOT SUPPRESS THE SWEEP. The
        # confirmation exists to stop a repository-wide prune firing on
        # suspicion alone, so its failure mode is the dangerous one: read as
        # "nothing of ours is registered" it would skip the sweep exactly when
        # the gate cannot see what it left behind. Forced by replacing the
        # listing with a failing command, which is also the shape a broken pipe
        # produced before the pipeline was removed (Codex adversarial, section
        # 54 round 4, [medium]).
        G_MUT_LIST="$GATE_REPO/scripts/todo-graph/identity-gate-nolist.sh"
        # ANCHORED ON THE ASSIGNMENT, NOT THE WHOLE LINE. Section 55 appended a
        # `fixed-allowance:` marker to this command's line, and a `$`-anchored
        # pattern stopped matching -- so the mutant came back identical to its
        # source and the case correctly refused to draw a verdict from it. The
        # anchor now ends at the substitution it replaces.
        sed 's#^        _wt_list="\$(timeout --foreground -s KILL 60 git worktree list --porcelain 2>/dev/null)"#        _wt_list="$(false)"#' \
            "$GATE_IN_CLONE" > "$G_MUT_LIST"
        if ! g_mutant_ok "$GATE_IN_CLONE" "$G_MUT_LIST" '_wt_list="\$(false)"'; then
            t_fail "identity gate: could not build the unanswerable-listing mutation (the shape moved; the mutant is identical to its source)"
        else
            rm -f "$TMP_DIR/s54-adds-seen"
            # A BYSTANDER REGISTRATION THAT A PRUNE WOULD COLLECT. Its directory
            # is deleted, so it is prunable by git's own rule; nothing about it
            # belongs to this gate.
            G_S54_BYST="$TMP_DIR/s54-bystander-wt"
            rm -rf "$G_S54_BYST"
            (cd "$GATE_REPO" && git worktree add --detach "$G_S54_BYST" HEAD >/dev/null 2>&1)
            rm -rf "$G_S54_BYST"
            (cd "$GATE_REPO" && PATH="$G_SHIM_S54:$PATH" bash "$G_MUT_LIST" \
                --base "$G_S54_WTBASE" --head HEAD >"$TMP_DIR/gate-22em-nolist.log" 2>&1)
            G_S54_NL_STILL="$( (cd "$GATE_REPO" && git worktree list --porcelain 2>/dev/null \
                | grep -c '/base$\|/head$') || true )"
            G_S54_NL_BYSTANDER=0
            (cd "$GATE_REPO" && git worktree list --porcelain 2>/dev/null \
                | grep -qxF "worktree $G_S54_BYST") && G_S54_NL_BYSTANDER=1
            G_S54_NL_ADDS="$(grep -c . "$TMP_DIR/s54-adds-seen" 2>/dev/null || echo 0)"
            # AN UNANSWERABLE LISTING MUST NOT DESTROY ANYTHING. `prune` is
            # repository-wide, and git treats an unlocked worktree whose storage
            # is momentarily unavailable as prunable, so sweeping on a failed
            # enumeration can deregister somebody else's live worktree. The
            # bystander here stands in for exactly that: its directory is gone,
            # so a prune WOULD collect it, and it must survive.
            if [ "${G_S54_NL_ADDS:-0}" -ge 1 ] && [ "${G_S54_NL_BYSTANDER:-0}" -eq 1 ]; then
                t_pass "identity gate: a listing the gate cannot read prunes nothing, so an unrelated registration survives"
            else
                t_fail "identity gate: an unanswerable worktree listing swept the whole repository (adds=$G_S54_NL_ADDS bystander=$G_S54_NL_BYSTANDER still=$G_S54_NL_STILL; see $TMP_DIR/gate-22em-nolist.log)"
            fi
            rm -f "$G_MUT_LIST"
        fi
        # 22en: A CHECKOUT FILTER MUST NOT DECIDE WHAT THE GATE EXECUTES. A
        # smudge filter rewrites bytes on the way OUT of git and its clean
        # counterpart rewrites them back on the way in, so `hash-object --path`
        # and `status` both round-trip to the stored blob and report the tree
        # faithful -- while bash and python run the smudged bytes. It is the
        # same shape as symlink emulation: faithful by git's measure, different
        # by the only measure that matters here. Disabling hooks does nothing
        # about it (Codex adversarial, section 54 post-ship, [high]).
        # THE FILTER PAIR ROUND-TRIPS, and that is the whole point of the case.
        # A smudge whose clean does NOT undo it leaves the tree reporting
        # MODIFIED, so `status` catches it and the run never reaches the check
        # this case is about -- which is exactly what the first draft measured.
        # The dangerous pair is the one git considers clean: smudge adds a line
        # on the way out, clean removes it on the way in, `hash-object --path`
        # and `status` both round-trip to the stored blob, and only the
        # attribute itself reveals that the executed bytes are not the commit's.
        G_S54_SMUDGE="$TMP_DIR/s54-smudge.sh"
        G_S54_CLEAN="$TMP_DIR/s54-clean.sh"
        printf '#!/bin/sh\ncat\necho "# s54smudge"\n' > "$G_S54_SMUDGE"
        printf '#!/bin/sh\nsed "/^# s54smudge$/d"\n' > "$G_S54_CLEAN"
        chmod +x "$G_S54_SMUDGE" "$G_S54_CLEAN"
        (
            cd "$GATE_REPO" || exit 1
            git checkout --quiet HEAD -- . >/dev/null 2>&1
            git config filter.s54evil.smudge "$G_S54_SMUDGE"
            git config filter.s54evil.clean "$G_S54_CLEAN"
            printf 'scripts/todo-graph/resolve_symbol.py filter=s54evil\n' > .gitattributes
            git add .gitattributes >/dev/null 2>&1
            printf '\n# 22en: a closure edit so the range is adjudicated\n' \
                >> scripts/todo-graph/ref_resolution.py
            git add scripts/todo-graph/ref_resolution.py >/dev/null 2>&1
            git commit --quiet --no-verify -m "22en: a filter over a closure member" >/dev/null 2>&1
        )
        G_S54_FLT_BASE="$( (cd "$GATE_REPO" && git rev-parse HEAD~1) )"
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_S54_FLT_BASE" --head HEAD \
            >"$TMP_DIR/gate-22en.log" 2>&1)
        G_S54_FLT_RC=$?
        G_S54_FLT_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22en.log" | head -1)"
        G_S54_FLT_ATTR="$( (cd "$GATE_REPO" && git check-attr filter -- scripts/todo-graph/resolve_symbol.py) )"
        # PREMISE: the attribute really is in force, or this case proves only
        # that an unfiltered tree passes.
        case "$G_S54_FLT_ATTR" in
            *s54evil*) G_S54_FLT_SET=yes ;;
            *) G_S54_FLT_SET=no ;;
        esac
        if [ "$G_S54_FLT_SET" = yes ] && [ "$G_S54_FLT_RC" -eq 3 ] \
           && grep -q 'MATERIALIZATION_UNFAITHFUL' "$TMP_DIR/gate-22en.log" \
           && grep -q 'checkout filter' "$TMP_DIR/gate-22en.log"; then
            t_pass "identity gate: a checkout filter over a closure member is refused, not executed"
        else
            t_fail "identity gate: a checkout filter reached the executed bytes (rc=$G_S54_FLT_RC; attr='$G_S54_FLT_ATTR'; got [$G_S54_FLT_TOKEN]; see $TMP_DIR/gate-22en.log)"
        fi
        # CONTROL: the same range with the attribute gone reaches an ordinary
        # verdict, so the refusal is about the filter and not about the commit.
        (
            cd "$GATE_REPO" || exit 1
            git rm --quiet -f .gitattributes >/dev/null 2>&1
            git commit --quiet --no-verify -m "22en: drop the filter attribute" >/dev/null 2>&1
            git config --unset filter.s54evil.smudge >/dev/null 2>&1
            git config --unset filter.s54evil.clean >/dev/null 2>&1
            git checkout --quiet HEAD -- . >/dev/null 2>&1
        )
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_S54_FLT_BASE" --head HEAD \
            >"$TMP_DIR/gate-22en-control.log" 2>&1)
        if ! grep -q 'MATERIALIZATION_UNFAITHFUL' "$TMP_DIR/gate-22en-control.log"; then
            t_pass "identity gate: CONTROL -- with the filter attribute gone the same range is adjudicated normally"
        else
            t_fail "identity gate: the filter refusal fires without a filter (see $TMP_DIR/gate-22en-control.log)"
        fi

        # AND THE CONFIRMATION SURVIVES A TMPDIR CONTAINING A SPACE. `mktemp -t`
        # honours TMPDIR, so this is an ordinary path, not an exotic one -- and
        # while the attempted paths were held in a space-separated string they
        # word-split, so neither was ever tested intact and the sweep silently
        # never fired (Codex adversarial, section 54 round 5, [medium]).
        G_S54_WSTMP="$TMP_DIR/s54 space tmp"
        rm -rf "$G_S54_WSTMP"; mkdir -p "$G_S54_WSTMP"
        rm -f "$TMP_DIR/s54-adds-seen"
        (cd "$GATE_REPO" && TMPDIR="$G_S54_WSTMP" PATH="$G_SHIM_S54:$PATH" bash "$GATE_IN_CLONE" \
            --base "$G_S54_WTBASE" --head HEAD >"$TMP_DIR/gate-22em-space.log" 2>&1)
        G_S54_WS_ADDS="$(grep -c . "$TMP_DIR/s54-adds-seen" 2>/dev/null || echo 0)"
        G_S54_WS_STILL="$( (cd "$GATE_REPO" && git worktree list --porcelain 2>/dev/null \
            | grep -c '/base$\|/head$') || true )"
        if [ "${G_S54_WS_ADDS:-0}" -ge 1 ] && [ "${G_S54_WS_STILL:-0}" -eq 0 ]; then
            t_pass "identity gate: a TMPDIR containing a space still confirms and sweeps its own registrations"
        else
            t_fail "identity gate: a whitespace TMPDIR defeated the attempted-path confirmation (adds=$G_S54_WS_ADDS still=$G_S54_WS_STILL; see $TMP_DIR/gate-22em-space.log)"
        fi
        rm -rf "$G_S54_WSTMP"
        rm -f "$TMP_DIR/s54-adds-seen"
        rm -rf "$G_SHIM_S54"
        (cd "$GATE_REPO" && git worktree prune --expire now >/dev/null 2>&1 || true)
    fi

    # ------------------------------------------------------------------
    # 22f1..22f3: EVERY PUBLISHED REFUSAL IS ONE SOMETHING CAN STILL
    # PROVOKE (section 58).
    #
    # Section 54 moved the head side to the commit and section 55 replaced the
    # full checkout with a narrow assembly, which between them changed the
    # INPUTS to three published tokens without changing the tokens. Section 58
    # measured what is left of each rather than deciding it by reading:
    #
    #   HEAD_PROTOCOL_UNMATERIALIZED  still reachable, but only under a
    #     CONJUNCTION the section was originally filed without. Both emitters
    #     sit under the `UNREADABLE|*"?"*` case, so `proto_of` must ALREADY
    #     have failed for a reason of its own; and every divergence that
    #     exists before section 55's re-binding is claimed by
    #     MATERIALIZATION_UNFAITHFUL instead. So the one surviving route is a
    #     tree that loses a member AFTER the re-binding verified it, which is
    #     what 22f1 constructs (Codex design review, section 58, [high]).
    #   the closure presence pair  gone: section 55 deleted both branches. A
    #     deleted branch has nothing to fixture and nothing to retire.
    #   CLOSURE_MOVED_UNDER_GATE  no emitter since section 55, and no input
    #     can bring one back, so it is a TOMBSTONE rather than a reserved
    #     guard -- 22f3 asserts statically that no emitter returned.
    #
    # THE SYNCHRONIZATION IS A BARRIER, NOT A SLEEP AND NOT A /proc POLL. The
    # window is a handful of shell commands wide, so a fixed sleep would be the
    # wall-clock shape section 34 forbids and this suite already flakes on;
    # polling /proc for the re-binding probe can miss a short-lived process,
    # and its exit does not schedule the descendant before bash reaches the
    # read anyway (Codex design review, section 58). A gate copy therefore
    # signals a FIFO once the re-binding has SUCCEEDED and blocks until the
    # descendant acknowledges. The mutation is the CLOCK: the classification
    # under test is untouched, and the descendant is a real escapee spawned by
    # real tree code, not a fixture writing the condition directly.
    # ------------------------------------------------------------------
    (cd "$GATE_REPO" && git checkout --quiet -- . >/dev/null 2>&1 || true)
    G_S58_FA="$TMP_DIR/s58-fifo-a"
    G_S58_FB="$TMP_DIR/s58-fifo-b"
    G_S58_MARK="$TMP_DIR/s58-child-pids"
    rm -f "$G_S58_FA" "$G_S58_FB" "$G_S58_MARK"
    mkfifo "$G_S58_FA" "$G_S58_FB" 2>/dev/null || true

    # THE BASE IS RESTORED FROM THE REAL REPOSITORY, not from whatever the
    # preceding cases left in the clone. A case that walks in with a broken
    # base refuses for a reason that has nothing to do with what it asserts,
    # and every assertion below is about a HEAD-side token.
    (
        cd "$GATE_REPO" || exit 1
        for _p in snapshot_protocol.json snapshot_protocol.py ref_resolution.py \
                  corpus_resolution_snapshot.py; do
            cp "$REPO_ROOT/scripts/todo-graph/$_p" "scripts/todo-graph/$_p" 2>/dev/null || true
        done
        git add -A scripts/todo-graph >/dev/null 2>&1
        git commit --quiet --no-verify -m "22f1: restore a sound protocol for the base" >/dev/null 2>&1
    )
    G_S58_BASE="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    G_S58_SEED=no
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_S58_BASE" --head HEAD \
        >"$TMP_DIR/gate-22f1-seed.log" 2>&1) && G_S58_SEED=yes
    if [ "$G_S58_SEED" != yes ]; then
        t_fail "identity gate: the section 58 fixtures cannot start -- the restored base does not adjudicate cleanly (see $TMP_DIR/gate-22f1-seed.log)"
    else
    # g_s58_loader <spawn?>: write a snapshot_protocol.py that DISAGREES with
    # the (still valid) JSON, so `proto_of` returns UNREADABLE at the
    # loader-versus-data comparison without raising -- which matters, because a
    # loader that raised might not have reached the spawn below. With `spawn`
    # it first starts a detached descendant; `start_new_session` is what puts
    # that child outside the probe's process group, which is precisely the
    # containment limit section 56 measured and left in place, so this fixture
    # is also a live regression test for that limit.
    g_s58_loader() {   # $1 = spawn|quiet, $2 = action python, $3 = destination
        {
            printf '%s\n' 'import os, subprocess, sys'
            if [ "$1" = spawn ]; then
                printf '%s\n' '_root = os.path.dirname(os.path.dirname(os.path.dirname('
                printf '%s\n' '    os.path.realpath(__file__))))'
                printf '%s\n' '_child = r"""'
                printf '%s\n' 'import os, signal, sys'
                printf '%s\n' '# BOUNDED: if nothing ever signals, this child must not outlive the'
                printf '%s\n' '# suite. The alarm is a failsafe, never the mechanism.'
                printf '%s\n' 'signal.alarm(300)'
                printf '%s\n' 'root = sys.argv[1]'
                printf '%s\n' 'with open(os.environ["S58_MARK"], "a") as _m:'
                printf '%s\n' '    _st = open("/proc/self/stat").read().split()[21]'
                printf '%s\n' '    _m.write("%d %s\\n" % (os.getpid(), _st))'
                printf '%s\n' 'with open(os.environ["S58_FA"]) as _f:'
                printf '%s\n' '    _f.read(1)'
                printf '%s\n' 'try:'
                printf '%s\n' "$2"
                printf '%s\n' '    open(os.environ["S58_MARK"] + ".acted", "w").close()'
                printf '%s\n' 'except Exception:'
                printf '%s\n' '    pass'
                printf '%s\n' 'with open(os.environ["S58_FB"], "w") as _f:'
                # THE ACK ENDS IN A NEWLINE. `read -r` returns NONZERO on
                # EOF without its delimiter even when it read the byte, so a
                # bare "a" made a successful rendezvous set the barrier-timeout
                # sentinel, and the synchronization precondition then refused a
                # case whose token had fired correctly.
                printf '%s\n' '    _f.write("a\n")'
                printf '%s\n' '"""'
                printf '%s\n' 'try:'
                printf '%s\n' '    subprocess.Popen([sys.executable, "-c", _child, _root],'
                printf '%s\n' '                     start_new_session=True,'
                printf '%s\n' '                     stdin=subprocess.DEVNULL,'
                printf '%s\n' '                     stdout=subprocess.DEVNULL,'
                printf '%s\n' '                     stderr=subprocess.DEVNULL)'
                printf '%s\n' 'except Exception:'
                printf '%s\n' '    pass'
            fi
            # DISAGREES WITH THE JSON rather than raising: identity-gate.sh
            # compares the loader's constants against the data file and reports
            # UNREADABLE on a mismatch, which is the independent protocol
            # failure this fixture needs and the cleanest one available.
            printf '%s\n' 'SNAPSHOT_SCHEMA = 99999'
            printf '%s\n' 'PRE_RESOLUTION_BUCKETS = ("s58_not_the_declared_bucket",)'
            printf '%s\n' 'POST_RESOLUTION_BUCKETS = ()'
            printf '%s\n' 'ALL_BUCKETS = PRE_RESOLUTION_BUCKETS + POST_RESOLUTION_BUCKETS'
        } > "$3"
    }

    # g_s58_barrier <dest> [extra-python-sed]: the CLOCK. Inserted immediately
    # before the head-side worktree read, which is the one point that is after
    # section 55's re-binding and before the classification consumes the tree.
    # A BARRIER THAT TIMED OUT IS A FIXTURE FAILURE, NOT A SLOWER PASS. With
    # the timeouts swallowed, a run where the descendant never arrived simply
    # proceeded to an UNSYNCHRONIZED observation, and the case then reported
    # whatever that produced (Codex adversarial, section 58, [medium]). Each
    # side now leaves a sentinel when its timeout fires, and the cases refuse.
    g_s58_barrier() {   # $1 = destination
        python3 - "$GATE_IN_CLONE" "$1" "$G_S58_FA" "$G_S58_FB" "$G_S58_MARK" <<'S58BAR'
import sys
src, dst, fa, fb, mark = sys.argv[1:6]
text = open(src, encoding="utf-8").read()
anchor = '        protocol_present_in_worktree "$HEAD_MIN_ADDR"\n'
if text.count(anchor) != 1:
    sys.exit(1)


def q(p):
    return "'" + p.replace("'", "'\\''") + "'"


# THE BOUND IS A FAILSAFE AGAINST AN ABSENT CHILD, NOT A PERFORMANCE
# ASSERTION, so it is generous. A rendezvous that works returns at once and
# pays none of it, while a 30s bound only ever measured how loaded the host
# was -- and this suite runs inside a tooling pack beside every other suite,
# which is the wall-clock shape section 34 forbids and this file already
# carries documented flakes from.
bar = (
    "        if timeout 90 sh -c 'printf r > \"$0\"' %s >/dev/null 2>&1; then\n"
    "            timeout 90 sh -c 'read -r _x < \"$0\"' %s >/dev/null 2>&1 \\\n"
    "                || : > %s\n"
    "        else\n"
    "            : > %s\n"
    "        fi\n"
) % (q(fa), q(fb), q(mark + ".barrier-b-timeout"),
     q(mark + ".barrier-a-timeout"))
open(dst, "w", encoding="utf-8").write(text.replace(anchor, bar + anchor))
S58BAR
    }

    # g_s58_synced: did the barrier actually rendezvous, and did the descendant
    # start AND complete its action? Asserted as a PRECONDITION of every token
    # claim below rather than printed in the failure text, which is where the
    # first cut left it -- so any other removal of the protocol path during the
    # window could produce the expected token and pass.
    g_s58_synced() {
        [ ! -f "$G_S58_MARK.barrier-a-timeout" ] \
            && [ ! -f "$G_S58_MARK.barrier-b-timeout" ] \
            && [ -s "$G_S58_MARK" ] \
            && [ -f "$G_S58_MARK.acted" ]
    }

    # THE DESCENDANTS ARE REAPED BY EXACT PID AND START TIME, never by a
    # command-line match: a global sweep would kill another concurrent run's
    # child, which is the discipline 22eq and the section 56 fixtures were
    # repaired into having.
    g_s58_reap() {
        [ -f "$G_S58_MARK" ] || return 0
        while IFS=" " read -r _s58p _s58st; do
            [ -n "$_s58p" ] && [ -n "$_s58st" ] || continue
            _s58now="$(awk '{print $22}' "/proc/$_s58p/stat" 2>/dev/null || true)"
            if [ -n "$_s58now" ] && [ "$_s58now" = "$_s58st" ]; then
                kill -KILL "$_s58p" 2>/dev/null || true
                for _s58i in 1 2 3 4 5 6 7 8 9 10; do
                    kill -0 "$_s58p" 2>/dev/null || break
                    sleep 0.2
                done
            fi
        done < "$G_S58_MARK"
    }

    # EVERY CASE STARTS FROM NO EVIDENCE. Left in place, 22f1's marker and
    # `.acted` file would satisfy 22f2's synchronization precondition without
    # 22f2's own descendant ever running -- the evidence check would then be
    # reading the previous case.
    g_s58_reset() {
        g_s58_reap
        rm -f "$G_S58_MARK" "$G_S58_MARK.acted" \
              "$G_S58_MARK.barrier-a-timeout" "$G_S58_MARK.barrier-b-timeout"
    }

        # 22f1: THE ONE SURVIVING ROUTE TO HEAD_PROTOCOL_UNMATERIALIZED.
        G_S58_ACT='    os.remove(os.path.join(root, "scripts/todo-graph/snapshot_protocol.json"))'
        g_s58_loader spawn "$G_S58_ACT" "$GATE_REPO/scripts/todo-graph/snapshot_protocol.py"
        (
            cd "$GATE_REPO" || exit 1
            git commit --quiet --no-verify -am "22f1: a loader that disagrees with the data and leaves a descendant" >/dev/null 2>&1
        )
        G_S58_BAR="$GATE_REPO/scripts/todo-graph/identity-gate-s58bar.sh"
        rm -f "$G_S58_BAR"
        g_s58_barrier "$G_S58_BAR"
        if ! g_mutant_ok "$GATE_IN_CLONE" "$G_S58_BAR" 'read -r _x <'; then
            t_fail "identity gate: could not build the section 58 barrier copy -- the head-side worktree read moved, so 22f1 cannot be trusted"
        else
            g_s58_reset
            (cd "$GATE_REPO" && S58_FA="$G_S58_FA" S58_FB="$G_S58_FB" S58_MARK="$G_S58_MARK" \
                bash "$G_S58_BAR" --base "$G_S58_BASE" --head HEAD \
                >"$TMP_DIR/gate-22f1.log" 2>&1)
            G_S58_RC=$?
            # THE DESCENDANT IS ASSERTED SEPARATELY FROM THE TOKEN, so a case
            # that fails says WHICH half failed. A child that never ran and a
            # child that ran at the wrong moment produce the same missing token
            # and need opposite repairs.
            G_S58_TOK="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22f1.log" | head -1)"
            G_S58_RAN=no
            [ -s "$G_S58_MARK" ] && G_S58_RAN=yes
            G_S58_ACTED=no
            [ -f "$G_S58_MARK.acted" ] && G_S58_ACTED=yes
            G_S58_SYNC=no
            g_s58_synced && G_S58_SYNC=yes
            # MATCHED ON THE EMITTED TOKEN, NOT ON THE NAME ANYWHERE IN THE
            # OUTPUT. `die_infra` prints `INFRASTRUCTURE: <token>: ...`, and
            # the first cut of this assertion excluded the bare string
            # MATERIALIZATION_UNFAITHFUL -- which the HEAD_PROTOCOL_UNMATERIALIZED
            # message itself now contains, because that message explains which
            # token would have claimed an earlier divergence. So the case failed
            # while the token it wanted had fired correctly. A refusal is the
            # token line, never a word in the prose beside it.
            if [ "$G_S58_RC" -eq 3 ] \
               && [ "$G_S58_SYNC" = yes ] \
               && grep -q 'INFRASTRUCTURE: HEAD_PROTOCOL_UNMATERIALIZED' "$TMP_DIR/gate-22f1.log" \
               && grep -q 'snapshot_protocol.json' "$TMP_DIR/gate-22f1.log" \
               && ! grep -q 'INFRASTRUCTURE: MATERIALIZATION_UNFAITHFUL' "$TMP_DIR/gate-22f1.log"; then
                t_pass "identity gate: a probe descendant that outlives its process group still provokes HEAD_PROTOCOL_UNMATERIALIZED, so the token is not dead"
            else
                cp "$TMP_DIR/gate-22f1.log" "$TMP_DIR/../s58-22f1-kept.log" 2>/dev/null || true
                t_fail "identity gate: the surviving route to HEAD_PROTOCOL_UNMATERIALIZED did not fire (rc=$G_S58_RC; token=[$G_S58_TOK]; descendant ran=$G_S58_RAN acted=$G_S58_ACTED synced=$G_S58_SYNC barrier_a=$([ -f "$G_S58_MARK.barrier-a-timeout" ] && echo timeout || echo ok) barrier_b=$([ -f "$G_S58_MARK.barrier-b-timeout" ] && echo timeout || echo ok); kept at $TMP_DIR/../s58-22f1-kept.log)"
            fi
        fi

        # 22f1-control: THE INDEPENDENT PREMISE, ON ITS OWN. The same
        # disagreeing loader WITHOUT the descendant, run against the UNMUTATED
        # gate, must reach HEAD_PROTOCOL_UNREADABLE and not the token above.
        # Without this the case above could pass for the wrong reason -- an
        # UNREADABLE tree alone is not what the token is published to mean.
        g_s58_loader quiet '' "$GATE_REPO/scripts/todo-graph/snapshot_protocol.py"
        (
            cd "$GATE_REPO" || exit 1
            git commit --quiet --no-verify -am "22f1-control: the same disagreement, no descendant" >/dev/null 2>&1
        )
        (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" --base "$G_S58_BASE" --head HEAD \
            >"$TMP_DIR/gate-22f1-control.log" 2>&1)
        G_S58_CRC=$?
        if [ "$G_S58_CRC" -eq 3 ] \
           && grep -q 'INFRASTRUCTURE: HEAD_PROTOCOL_UNREADABLE' "$TMP_DIR/gate-22f1-control.log" \
           && ! grep -q 'INFRASTRUCTURE: HEAD_PROTOCOL_UNMATERIALIZED' "$TMP_DIR/gate-22f1-control.log"; then
            t_pass "identity gate: CONTROL -- the protocol failure alone reaches UNREADABLE, so 22f1 measures the tree change and not the loader"
        else
            t_fail "identity gate: the 22f1 control did not isolate the protocol failure (rc=$G_S58_CRC; see $TMP_DIR/gate-22f1-control.log)"
        fi

        # 22f2: THE SECOND EMITTER IS CONTAMINATION, AND IT NOW SAYS SO.
        #
        # The path loop above it has already refused every path the commit
        # carries and the tree lacks, so by the form comparison the tree holds
        # EVERYTHING the commit holds -- and the only way its form can still be
        # weaker is that it holds something MORE. A legacy-complete commit
        # whose assembled tree gains snapshot_protocol.json without the data
        # loader is that shape. Until section 58 it reported
        # HEAD_PROTOCOL_UNMATERIALIZED with "a partial clone missing the blob",
        # a cause that cannot produce this branch (Codex design review,
        # section 58, [medium]).
        (
            cd "$GATE_REPO" || exit 1
            git rm --quiet -f scripts/todo-graph/snapshot_protocol.json \
                scripts/todo-graph/snapshot_protocol.py >/dev/null 2>&1
            python3 - scripts/todo-graph/ref_resolution.py <<'S58REF'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
# A NON-SEQUENCE where the contract wants a sequence: the legacy arm of
# `proto_of` refuses that as UNREADABLE, which is this case's independent
# protocol failure. The spawn goes in the same file because the legacy arm is
# what imports it.
p.write_text(
    'import os, subprocess, sys\n'
    '_root = os.path.dirname(os.path.dirname(os.path.dirname(\n'
    '    os.path.realpath(__file__))))\n'
    '_child = r"""\n'
    'import json, os, signal, sys\n'
    'signal.alarm(300)\n'
    'root = sys.argv[1]\n'
    'with open(os.environ["S58_MARK"], "a") as _m:\n'
    '    _st = open("/proc/self/stat").read().split()[21]\n'
    '    _m.write("%d %s\\n" % (os.getpid(), _st))\n'
    'with open(os.environ["S58_FA"]) as _f:\n'
    '    _f.read(1)\n'
    'try:\n'
    '    with open(os.path.join(root, "scripts/todo-graph/snapshot_protocol.json"),\n'
    '              "w") as _j:\n'
    '        json.dump({"snapshot_schema": 2, "pre_resolution_buckets": [],\n'
    '                   "post_resolution_buckets": [], "renamed_buckets": {},\n'
    '                   "retired_buckets": []}, _j)\n'
    '    open(os.environ["S58_MARK"] + ".acted", "w").close()\n'
    'except Exception:\n'
    '    pass\n'
    'with open(os.environ["S58_FB"], "w") as _f:\n'
    '    _f.write("a\\n")\n'
    '"""\n'
    'try:\n'
    '    subprocess.Popen([sys.executable, "-c", _child, _root],\n'
    '                     start_new_session=True, stdin=subprocess.DEVNULL,\n'
    '                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)\n'
    'except Exception:\n'
    '    pass\n'
    'PRE_RESOLUTION_BUCKETS = 58\n'
    'POST_RESOLUTION_BUCKETS = 58\n',
    encoding="utf-8")
S58REF
            git add -A scripts/todo-graph >/dev/null 2>&1
            git commit --quiet --no-verify -m "22f2: a legacy-complete head whose tree gains the json" >/dev/null 2>&1
        )
        rm -f "$G_S58_BAR"
        g_s58_barrier "$G_S58_BAR"
        if ! g_mutant_ok "$GATE_IN_CLONE" "$G_S58_BAR" 'read -r _x <'; then
            t_fail "identity gate: could not rebuild the barrier copy for 22f2"
        else
            g_s58_reset
            (cd "$GATE_REPO" && S58_FA="$G_S58_FA" S58_FB="$G_S58_FB" S58_MARK="$G_S58_MARK" \
                bash "$G_S58_BAR" --base "$G_S58_BASE" --head HEAD \
                >"$TMP_DIR/gate-22f2.log" 2>&1)
            G_S58_RC2=$?
            G_S58_SYNC2=no
            g_s58_synced && G_S58_SYNC2=yes
            if [ "$G_S58_RC2" -eq 3 ] \
               && [ "$G_S58_SYNC2" = yes ] \
               && grep -q 'INFRASTRUCTURE: MATERIALIZATION_UNFAITHFUL' "$TMP_DIR/gate-22f2.log" \
               && grep -q 'GAINED a protocol path' "$TMP_DIR/gate-22f2.log" \
               && ! grep -q 'INFRASTRUCTURE: HEAD_PROTOCOL_UNMATERIALIZED' "$TMP_DIR/gate-22f2.log"; then
                t_pass "identity gate: a tree that GAINS a protocol path is reported as contamination, not as a partial clone"
            else
                cp "$TMP_DIR/gate-22f2.log" "$TMP_DIR/../s58-22f2-kept.log" 2>/dev/null || true
                t_fail "identity gate: the contamination route did not reclassify (rc=$G_S58_RC2; synced=$G_S58_SYNC2 barrier_a=$([ -f "$G_S58_MARK.barrier-a-timeout" ] && echo timeout || echo ok) barrier_b=$([ -f "$G_S58_MARK.barrier-b-timeout" ] && echo timeout || echo ok) ran=$([ -s "$G_S58_MARK" ] && echo yes || echo no) acted=$([ -f "$G_S58_MARK.acted" ] && echo yes || echo no); kept at $TMP_DIR/../s58-22f2-kept.log)"
            fi
            # MUTATION: put the old classification back and the same tree
            # reports the old, wrong token again -- so 22f2 measures the
            # reclassification and not some unrelated refusal.
            G_S58_OLD="$GATE_REPO/scripts/todo-graph/identity-gate-s58old.sh"
            rm -f "$G_S58_OLD"
            python3 - "$G_S58_BAR" "$G_S58_OLD" <<'S58OLD'
import sys
src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
needle = 'die_infra "MATERIALIZATION_UNFAITHFUL: the head tree assembled for $HEAD_RESOLVED holds every snapshot-protocol path'
i = text.find(needle)
if i < 0:
    sys.exit(1)
j = text.index("\n", i)
old = ('die_infra "HEAD_PROTOCOL_UNMATERIALIZED: the commit $HEAD_RESOLVED '
       'carries a complete snapshot protocol but the tree the gate assembled '
       'for it at $HEAD_MIN does not (missing$HEAD_MISSING)"')
open(dst, "w", encoding="utf-8").write(text[:i] + old + text[j:])
S58OLD
            if ! g_mutant_ok "$G_S58_BAR" "$G_S58_OLD" 'HEAD_PROTOCOL_UNMATERIALIZED: the commit \$HEAD_RESOLVED carries a complete'; then
                t_fail "identity gate: could not build the 22f2 reclassification mutant, so that case proves nothing"
            else
                g_s58_reset
                (cd "$GATE_REPO" && S58_FA="$G_S58_FA" S58_FB="$G_S58_FB" S58_MARK="$G_S58_MARK" \
                    bash "$G_S58_OLD" --base "$G_S58_BASE" --head HEAD \
                    >"$TMP_DIR/gate-22f2-mut.log" 2>&1)
                G_S58_MRC2=$?
                # THE MUTATION ARM IS SYNCHRONIZATION-GATED TOO. Asserting only
                # rc and the token let a timed-out barrier plus any other actor
                # changing the json in the observation window satisfy it, which
                # contradicts the guarantee the other arms carry (Codex
                # adversarial, section 58, [medium]). An invocation nonce was
                # considered and is not needed: `g_s58_reset` reaps the recorded
                # descendants by exact pid AND start time before clearing the
                # evidence, so a stale child cannot be the one that acted, and a
                # timed-out barrier now leaves a sentinel this check reads.
                G_S58_SYNCM=no
                g_s58_synced && G_S58_SYNCM=yes
                if [ "$G_S58_MRC2" -eq 3 ] \
                   && [ "$G_S58_SYNCM" = yes ] \
                   && grep -q 'INFRASTRUCTURE: HEAD_PROTOCOL_UNMATERIALIZED' "$TMP_DIR/gate-22f2-mut.log"; then
                    t_pass "identity gate: MUTATION -- restoring the old classification brings the wrong token back, so 22f2 measures the fix"
                else
                    cp "$TMP_DIR/gate-22f2-mut.log" "$TMP_DIR/../s58-22f2mut-kept.log" 2>/dev/null || true
                    t_fail "identity gate: the 22f2 mutation did not restore the old token (rc=$G_S58_MRC2; synced=$G_S58_SYNCM barrier_a=$([ -f "$G_S58_MARK.barrier-a-timeout" ] && echo timeout || echo ok) barrier_b=$([ -f "$G_S58_MARK.barrier-b-timeout" ] && echo timeout || echo ok); kept at $TMP_DIR/../s58-22f2mut-kept.log)"
                fi
            fi
        fi

        # 22f3: THE TOMBSTONE STAYS A TOMBSTONE.
        #
        # A retired name is only safe for a caller branching on the vocabulary
        # while nothing quietly starts emitting it again for a different fault
        # -- which is exactly what section 55 declined to do and recorded as
        # section 58's to settle. This is a STATIC assertion because there is
        # no input that can provoke the token: a firing fixture is not
        # available for it at any price, and inventing one would mean
        # resurrecting an emitter so a check could pass.
        # THE ORACLE CLASSIFIES EVERY REFERENCE, it does not match one spelling
        # of the emitter. Counting `die_infra "CLOSURE_MOVED_UNDER_GATE` was
        # blind to a single-quoted call, to the token held in a variable, and
        # to any other emission path -- so the suite could stay green after the
        # retired public name was reused, which is the exact regression this
        # case claims to prevent (Codex test-coverage, section 58, [medium]).
        # The rule is stronger and simpler: a retired token may appear in
        # COMMENTS, and nowhere else. Its honest limit is that a name assembled
        # from pieces at runtime is not statically detectable; the mutation
        # below pins the two shapes that are.
        g_s58_tomb_scan() {   # $1 = script; prints "<executable-refs>|<tombstone-entries>"
            python3 - "$1" <<'S58TOMB'
import sys
tok = "CLOSURE_MOVED_UNDER_GATE"

# AN EXACT ALLOWLIST, NOT A COMMENT RULE. Three designs were tried and two
# were MEASURABLY fail-open, which is why this ends as a literal list of the
# lines that may name the retired token:
#   * "the first non-blank character is #" -- Python lstrip() strips NBSP and
#     bash does not, so an NBSP-prefixed line reads as a comment here and as a
#     COMMAND to bash, printing the retired token from its own diagnostic;
#   * "classify the trailing comment segment" -- bash starts a comment only
#     where # begins a WORD, so an assignment of #TOKEN followed by a prefix
#     stripping expansion emits the token with no escape at all;
#   * and either of those still misses a token on the SECOND line of a
#     multi-line quoted string, which is ordinary shell, not obfuscation.
# Getting them right means carrying bash lexical state -- word boundaries,
# quotes across newlines, here-documents -- a shell lexer inside one fixture,
# which is section 59 open question for this whole file (Codex adversarial,
# section 58 rounds 3 to 5; every one of those shapes was measured, not
# argued).
#
# The allowlist has none of those failure modes: an occurrence is permitted
# only when its line is EXACTLY one of the documentation lines below, so any
# new reference at all -- comment, quoted, spliced or executable -- is
# reported. The cost is that rewording one of these lines fails this case
# until the list is updated, and that is the intended bargain: re-blessing the
# prose about a retired token should be a deliberate act.
#
# AND WHAT IT PROVES IS BOUNDED, stated here rather than left to be inferred
# from the case name: no reference OUTSIDE the blessed lines, and every
# blessed line byte-exact. It does NOT prove "no executable reference exists",
# because a mutant can leave a blessed line untouched and open a multi-line
# quote AROUND it from the line before, which no line-equality rule can see
# (Codex adversarial, section 58 post-ship review, [medium], measured). That
# is the same shell-lexing boundary as the two evasions above, owned by
# section 59 rather than answered with a third rule here -- so the claim is
# narrowed to match instead of the check being widened to bluff.
ALLOWED = [
    '#        CLOSURE_MOVED_UNDER_GATE     RETIRED (section 55, recorded here by',
    '# before. Collapsing the two makes `CLOSURE_MOVED_UNDER_GATE` compare a value',
    "# Section 51's CLOSURE_MOVED_UNDER_GATE was not retired by that -- it guarded",
    '#   CLOSURE_MOVED_UNDER_GATE  a member changed WHILE the gate ran -- re-run',
    '# is what `CLOSURE_MOVED_UNDER_GATE` is published as meaning, and quietly',
    '#    left CLOSURE_MOVED_UNDER_GATE with no emitter at all, and section 58',
]

# CONTINUATIONS ARE SPLICED FIRST: a trailing backslash on one line and the
# token on the next is ONE assignment to bash. A comment line is never
# spliced, because bash ends a comment at the newline and joining one would
# swallow the line after it.
raw = open(sys.argv[1], encoding="utf-8").read().split("\n")
merged, buf = [], ""
for ln in raw:
    cur = buf + ln
    trailing = len(cur) - len(cur.rstrip("\\"))
    if trailing % 2 == 1 and not cur.lstrip(" \t").startswith("#"):
        buf = cur[:-1]
        continue
    merged.append(cur)
    buf = ""
if buf:
    merged.append(buf)

live, tomb = 0, 0
for ln in merged:
    if tok not in ln:
        continue
    t = ln.strip()
    if t in ALLOWED:
        if tok + "     RETIRED" in t:
            tomb += 1
        continue
    live += 1
print("%d|%d" % (live, tomb))
S58TOMB
        }
        G_S58_SCAN="$(g_s58_tomb_scan "$GATE_IN_CLONE")"
        G_S58_EMIT="${G_S58_SCAN%%|*}"; G_S58_TOMB="${G_S58_SCAN#*|}"
        if [ "${G_S58_EMIT:-1}" -ne 0 ]; then
            t_fail "identity gate: CLOSURE_MOVED_UNDER_GATE is referenced from $G_S58_EMIT executable line(s) -- a retired token was reused rather than a new one published"
        elif [ "${G_S58_TOMB:-0}" -lt 1 ]; then
            t_fail "identity gate: the CLOSURE_MOVED_UNDER_GATE tombstone entry is gone from the published vocabulary, so the name can be silently reused"
        else
            t_pass "identity gate: CLOSURE_MOVED_UNDER_GATE stays a published tombstone, named only on its blessed documentation lines"
        fi
        # AND THE ORACLE IS MUTATION-PROVED, because a static check that cannot
        # fail is not a check. Two plausible reintroductions are planted -- a
        # single-quoted emitter and one whose token travels in a variable --
        # and each must be seen.
        G_S58_TOMBMUT="$GATE_REPO/scripts/todo-graph/identity-gate-s58tomb.sh"
        for _s58shape in quoted variable escaped wordstart continuation nbsp mlquote; do
            rm -f "$G_S58_TOMBMUT"
            python3 - "$GATE_IN_CLONE" "$G_S58_TOMBMUT" "$_s58shape" <<'S58PLANT'
import sys
src, dst, shape = sys.argv[1], sys.argv[2], sys.argv[3]
lines = open(src, encoding="utf-8").read().split("\n")
# ANCHORED ON A LINE, NOT ON A HAND-ESCAPED LITERAL. The first cut matched a
# python string containing a real newline against a gate line containing the
# two characters backslash-n, so it never matched, the mutant was never
# written, and both arms reported only that they could not build it.
at = [i for i, ln in enumerate(lines) if ln.startswith("log() {")]
if len(at) != 1:
    sys.exit(1)
if shape == "quoted":
    plant = ("s58_reintroduced() { die_infra "
             "'CLOSURE_MOVED_UNDER_GATE: a member changed while the gate ran'; }")
elif shape == "variable":
    plant = ("s58_reintroduced() { _t=CLOSURE_MOVED_UNDER_GATE; "
             "die_infra \"$_t: a member changed while the gate ran\"; }")
elif shape == "escaped":
    # THE ESCAPED-HASH SHAPE, missed until section 58 round 3: the shell strips
    # the escape and the expansion strips the hash, so what reaches `die_infra`
    # is exactly the retired token.
    plant = ("s58_reintroduced() { _t=\\#CLOSURE_MOVED_UNDER_GATE; "
             "die_infra \"${_t#?}: a member changed while the gate ran\"; }")
elif shape == "wordstart":
    # THE SIMPLEST SHAPE, and the one the segment scanner missed: no escape at
    # all. Bash starts a comment only where "#" begins a WORD, so this hash is
    # ordinary text inside an assignment (Codex adversarial, round 4).
    plant = ("s58_reintroduced() { _t=#CLOSURE_MOVED_UNDER_GATE; "
             "die_infra \"${_t#?}: a member changed while the gate ran\"; }")
elif shape == "continuation":
    # SPLIT ACROSS A BACKSLASH-NEWLINE, so the physical line carrying the token
    # opens with "#" and reads as a comment while bash sees one assignment.
    plant = ("s58_reintroduced() { _t=\\\n"
             "#CLOSURE_MOVED_UNDER_GATE; "
             "die_infra \"${_t#?}: a member changed while the gate ran\"; }")
elif shape == "nbsp":
    # A NO-BREAK SPACE BEFORE THE HASH. Python lstrip() strips it and bash does
    # not, so this looks like a comment to a naive scanner and is a COMMAND to
    # the shell (Codex adversarial, round 5, measured).
    plant = ("\u00a0#CLOSURE_MOVED_UNDER_GATE is not a comment to bash\n"
             "s58_reintroduced() { :; }")
else:
    # THE TOKEN ON THE SECOND LINE OF A MULTI-LINE QUOTED STRING: the line
    # begins with "#" and is nonetheless inside an executable argument.
    plant = ("s58_reintroduced() { die_infra 'prefix\n"
             "#CLOSURE_MOVED_UNDER_GATE: reused'; }")
lines.insert(at[0] + 1, plant)
open(dst, "w", encoding="utf-8").write("\n".join(lines))
S58PLANT
            if ! g_mutant_ok "$GATE_IN_CLONE" "$G_S58_TOMBMUT" 's58_reintroduced'; then
                t_fail "identity gate: could not plant the $_s58shape CLOSURE_MOVED_UNDER_GATE emitter, so 22f3's oracle is unproven"
            else
                G_S58_MSCAN="$(g_s58_tomb_scan "$G_S58_TOMBMUT")"
                if [ "${G_S58_MSCAN%%|*}" -ge 1 ]; then
                    t_pass "identity gate: MUTATION -- the $_s58shape reintroduction of the retired token is seen by 22f3's oracle"
                else
                    t_fail "identity gate: the $_s58shape reintroduction of CLOSURE_MOVED_UNDER_GATE went unseen (scan=$G_S58_MSCAN), so 22f3 cannot catch the regression it exists for"
                fi
            fi
        done
        # AND THE STRICTNESS IS ASSERTED, not left to be discovered. Under the
        # allowlist even a well-meaning NEW COMMENT naming the retired token is
        # reported, because "is this line a comment?" is exactly the question no
        # line-local rule can answer about shell -- two attempts at one were
        # measurably fail-open. That is a deliberate bargain rather than an
        # oversight, so it gets a case: whoever adds prose about this token adds
        # the line to ALLOWED in the same edit, and this failure text says so.
        rm -f "$G_S58_TOMBMUT"
        python3 - "$GATE_IN_CLONE" "$G_S58_TOMBMUT" <<'S58NOTE'
import sys
src, dst = sys.argv[1], sys.argv[2]
lines = open(src, encoding="utf-8").read().split("\n")
at = [i for i, ln in enumerate(lines) if ln.startswith("log() {")]
if len(at) != 1:
    sys.exit(1)
lines.insert(at[0] + 1,
            's58_note() { die_infra "SOME_OTHER_TOKEN: unrelated"; }\n'
            '# CLOSURE_MOVED_UNDER_GATE stays retired, and a full comment'
            ' line is how you say so')
open(dst, "w", encoding="utf-8").write("\n".join(lines))
S58NOTE
        if ! g_mutant_ok "$GATE_IN_CLONE" "$G_S58_TOMBMUT" 's58_note'; then
            t_fail "identity gate: could not build the unblessed-mention case for 22f3"
        else
            G_S58_NSCAN="$(g_s58_tomb_scan "$G_S58_TOMBMUT")"
            if [ "${G_S58_NSCAN%%|*}" -ge 1 ]; then
                t_pass "identity gate: an UNBLESSED new mention of the retired token is reported even as a plain comment, which is the allowlist bargain"
            else
                t_fail "identity gate: 22f3's oracle admitted an unblessed new mention (scan=$G_S58_NSCAN) -- a line-local comment rule is back, and no such rule can see a token inside a multi-line quote"
            fi
        fi
        rm -f "$G_S58_TOMBMUT"

        g_s58_reset
        rm -f "$G_S58_FA" "$G_S58_FB" "$G_S58_BAR" "$G_S58_OLD"
        (
            cd "$GATE_REPO" || exit 1
            for _p in snapshot_protocol.json snapshot_protocol.py ref_resolution.py \
                      corpus_resolution_snapshot.py; do
                cp "$REPO_ROOT/scripts/todo-graph/$_p" "scripts/todo-graph/$_p" 2>/dev/null || true
            done
            git add -A scripts/todo-graph >/dev/null 2>&1
            git commit --quiet --no-verify -m "22f3: restore the protocol after the section 58 cases" >/dev/null 2>&1
        )
    fi

    # ------------------------------------------------------------------
    # 22f4: THE FILE MAY NOT CONTAIN A BACKTICK OR A PROCESS SUBSTITUTION
    # (section 59).
    #
    # Section 59 decided 22er's (the launch inventory) and 22f3's (the
    # tombstone scanner) shared parse boundary the cheap way rather than the
    # expensive one: neither shape is used anywhere in this file today, so
    # refusing them outright is a REGRESSION GUARD, not new capability, and it
    # closes 22er's own documented gap for good -- a backtick or `<( )`/`>( )`
    # launch can no longer be written here without this case catching it.
    #
    # ARITHMETIC EXPANSION STAYS LEGAL, on purpose: `$(( ... ))` is used six
    # times in this file and spawns nothing, and this scanner's frame rule --
    # borrowed byte-for-byte from 22er's `command_pieces`, itself
    # mutation-tested across five adversarial rounds -- treats `$((` as an
    # ordinary `$(` frame open, so an arithmetic expression is never itself
    # mistaken for a launch.
    #
    # WHAT THIS DOES NOT CLOSE: 22f3's own residual gap -- a mutant that opens
    # a multi-line quote AROUND a blessed line from the line before -- is NOT
    # fixed by banning multi-line quoted strings. They are common and
    # necessary here (every `"$(timeout ... \` continued onto the next line
    # before its closing `)"` is one), so forbidding them would be an invasive
    # reformat of the file, not a lint. Closing that gap for real needs the
    # shell lexer this section declined to build (a project, not a patch, per
    # 22er's own header); 22f3's exact-line allowlist remains the accepted,
    # narrower bound for that one.
    g_s59_shape_scan() {   # $1 = script; prints "<line>:<kind>[|...]" or CLEAN
        python3 - "$1" <<'S59SHAPE'
import re, sys

def scan(path):
    text = open(path, encoding="utf-8").read()
    findings = []
    stack = [{"s": False, "d": False, "extra": 0}]
    i, n = 0, len(text)
    line = 1
    pending_heredocs = []
    while i < n:
        ch = text[i]
        top = stack[-1]
        if ch == "\n":
            line += 1
            hd_start = i
            for tag in pending_heredocs:
                j = text.find("\n" + tag + "\n", i)
                end_j = text.find("\n", i + 1) if j == -1 else j + len(tag) + 1
                i = end_j if end_j != -1 else n
            line += text.count("\n", hd_start, i)
            pending_heredocs = []
            i += 1
            continue
        if ch == "\\" and not top["s"]:
            if i + 1 < n and text[i + 1] == "\n":
                i += 2
                line += 1
                continue
            i += 2
            continue
        if ch == "'" and not top["d"]:
            top["s"] = not top["s"]
            i += 1
            continue
        if ch == '"' and not top["s"]:
            top["d"] = not top["d"]
            i += 1
            continue
        if not top["s"] and text.startswith("$(", i):
            stack.append({"s": False, "d": False, "extra": 0})
            i += 2
            continue
        if ch == "`" and not top["s"]:
            findings.append((line, "backtick"))
            i += 1
            continue
        if not top["s"] and not top["d"]:
            if text.startswith("<(", i) or text.startswith(">(", i):
                findings.append((line, "process-substitution"))
                # ITS OWN PAREN NEEDS A MATCHING CLOSE, tracked the same as a
                # bare subshell below -- see the note there for why.
                top["extra"] += 1
                i += 2
                continue
            if ch == "#":
                rest_end = text.find("\n", i)
                rest_end = n if rest_end == -1 else rest_end
                i = rest_end
                continue
            if ch == "(":
                # A BARE GROUPING PAREN -- a subshell `(...)`, or the tail of
                # a process substitution already counted above. ITS `)` MUST
                # NOT BE MISTAKEN FOR THE ENCLOSING `$(` FRAME'S: popping the
                # substitution frame on this inner `)` let everything after
                # it re-enter the OUTER quote state early, which hid a LATER
                # `<( )` on the same logical line from this scanner entirely
                # (Codex adversarial, section 59, [high], measured against
                # `x="$( (echo first); diff <(echo a) <(echo b) )"`).
                top["extra"] += 1
                i += 1
                continue
            if ch == ")":
                if top["extra"] > 0:
                    top["extra"] -= 1
                elif len(stack) > 1:
                    stack.pop()
                i += 1
                continue
            # HEREDOC TAGS COME IN THREE LEGAL SHAPES -- unquoted, single- or
            # double-quoted. Recognising only the first two let a `<<"TAG"`
            # body scan as ordinary code, and a literal backtick inside that
            # body (e.g. prose inside an embedded python heredoc) then read
            # as a live launch (Codex adversarial, section 59, [medium],
            # measured).
            m = re.match(r"""<<-?(?:'([A-Za-z0-9_]+)'|"([A-Za-z0-9_]+)"|([A-Za-z0-9_]+))""",
                         text[i:])
            if m:
                pending_heredocs.append(next(g for g in m.groups() if g))
                i += m.end()
                continue
        i += 1
    return findings

f = scan(sys.argv[1])
print("CLEAN" if not f else "|".join("%d:%s" % (ln, k) for ln, k in f))
S59SHAPE
    }
    G_S59_SCAN="$(g_s59_shape_scan "$GATE_IN_CLONE")"
    if [ "$G_S59_SCAN" = "CLEAN" ]; then
        t_pass "identity gate: no backtick or process substitution anywhere in the file, so 22er's and 22f3's shared parse boundary cannot be exploited by either shape"
    else
        t_fail "identity gate: a backtick or process substitution reached identity-gate.sh ($G_S59_SCAN) -- the launch inventory and the tombstone scanner cannot see either shape"
    fi

    # MUTATION-PROVED: a planted launch of each forbidden shape must be seen,
    # including the one Codex's adversarial pass found this scanner missing:
    # a process substitution written AFTER an inner subshell on the SAME
    # substitution frame, which used to close that frame early (on the
    # subshell's own `)`) and blind the scanner to everything after it.
    for _s59shape in backtick process-substitution process-substitution-nested; do
        G_S59_MUT="$GATE_REPO/scripts/todo-graph/identity-gate-s59shape.sh"
        rm -f "$G_S59_MUT"
        python3 - "$GATE_IN_CLONE" "$G_S59_MUT" "$_s59shape" <<'S59PLANT'
import sys
src, dst, shape = sys.argv[1], sys.argv[2], sys.argv[3]
lines = open(src, encoding="utf-8").read().split("\n")
at = [i for i, ln in enumerate(lines) if ln.startswith("log() {")]
if len(at) != 1:
    sys.exit(1)
if shape == "backtick":
    plant = "s59_reintroduced() { X=`date +%s`; echo \"$X\"; }"
elif shape == "process-substitution":
    plant = "s59_reintroduced() { diff <(echo a) <(echo b) >/dev/null; }"
else:
    plant = ('s59_reintroduced() { x="$( (echo first); diff <(echo a)'
              ' <(echo b) )"; }')
lines.insert(at[0] + 1, plant)
open(dst, "w", encoding="utf-8").write("\n".join(lines))
S59PLANT
        if ! g_mutant_ok "$GATE_IN_CLONE" "$G_S59_MUT" 's59_reintroduced'; then
            t_fail "identity gate: could not plant the $_s59shape shape, so 22f4's oracle is unproven"
        else
            G_S59_MSCAN="$(g_s59_shape_scan "$G_S59_MUT")"
            case "$G_S59_MSCAN" in
                *process-substitution*|*backtick*)
                    t_pass "identity gate: MUTATION -- a planted $_s59shape launch is caught by 22f4's scanner" ;;
                *)
                    t_fail "identity gate: a planted $_s59shape launch went unseen ($G_S59_MSCAN), so 22f4 cannot catch the regression it exists for" ;;
            esac
        fi
        rm -f "$G_S59_MUT"
    done

    # CONTROL, the other direction: a double-quoted heredoc delimiter must
    # NOT be misread as ordinary code. A backtick inside the BODY of a
    # `<<"TAG"` heredoc is inert prose, never executed as shell -- the
    # earlier regex understood only unquoted and single-quoted tags, so this
    # shape false-positived (Codex adversarial, section 59, [medium]).
    G_S59_HDQ="$GATE_REPO/scripts/todo-graph/identity-gate-s59hdq.sh"
    rm -f "$G_S59_HDQ"
    python3 - "$GATE_IN_CLONE" "$G_S59_HDQ" <<'S59HDQ'
import sys
src, dst = sys.argv[1], sys.argv[2]
lines = open(src, encoding="utf-8").read().split("\n")
at = [i for i, ln in enumerate(lines) if ln.startswith("log() {")]
if len(at) != 1:
    sys.exit(1)
plant = ('s59_hdq_control() { cat <<"S59HDQBODY"\n'
         'this line names a `backtick` only in prose, inside a\n'
         'double-quoted heredoc body -- never executed as shell\n'
         'S59HDQBODY\n'
         '}')
lines.insert(at[0] + 1, plant)
open(dst, "w", encoding="utf-8").write("\n".join(lines))
S59HDQ
    if ! g_mutant_ok "$GATE_IN_CLONE" "$G_S59_HDQ" 's59_hdq_control'; then
        t_fail "identity gate: could not plant the double-quoted-heredoc control, so its false-positive risk is unproven"
    else
        G_S59_HDQSCAN="$(g_s59_shape_scan "$G_S59_HDQ")"
        if [ "$G_S59_HDQSCAN" = "CLEAN" ]; then
            t_pass "identity gate: a backtick inside a double-quoted heredoc body scans CLEAN, not as a false-positive launch"
        else
            t_fail "identity gate: a double-quoted heredoc's own body was misread as code ($G_S59_HDQSCAN) -- the heredoc-tag regex regressed"
        fi
    fi
    rm -f "$G_S59_HDQ"

    # ------------------------------------------------------------------
    # 22f5: A SECOND HEAD_TREE_UNREADABLE PROBE, DISTINCT FROM THE
    # ASSEMBLER'S (section 59).
    #
    # 22ba/22cc already prove the assembler's own probe (`materialize_subtree`,
    # identity-gate.sh:2575): they fail EVERY `ls-tree` naming the head SHA,
    # which fires before `proto_of` is even called. This case leaves that call
    # untouched and instead fails only the LATER, PLAIN (non `-r`) `ls-tree`
    # call `min_tree_still_matches` makes to re-verify the assembled tree
    # still holds the commit's bytes AFTER `proto_of` already read it
    # (identity-gate.sh:2816, `_unreadable` bound to `HEAD_TREE_UNREADABLE`
    # for the head side at :2668-2669) -- a genuinely different probe, only
    # reachable once materialization has already succeeded.
    G_S59_HEAD="$( (cd "$GATE_REPO" && git rev-parse HEAD) )"
    G_SHIM_S59A="$TMP_DIR/gitshim-s59a"
    mkdir -p "$G_SHIM_S59A"
    {
        printf '#!/usr/bin/env bash\n'
        printf 'if [ "$1" = "ls-tree" ]; then\n'
        printf '  has_r=0\n'
        printf '  for a in "$@"; do [ "$a" = "-r" ] && has_r=1; done\n'
        printf '  if [ "$has_r" -eq 0 ]; then\n'
        printf '    for a in "$@"; do [ "$a" = "%s" ] && exit 128; done\n' "$G_S59_HEAD"
        printf '  fi\n'
        printf 'fi\n'
        printf 'exec %s "$@"\n' "$G_REAL_GIT"
    } > "$G_SHIM_S59A/git"
    chmod +x "$G_SHIM_S59A/git"
    (cd "$GATE_REPO" && PATH="$G_SHIM_S59A:$PATH" bash "$GATE_IN_CLONE" \
        --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22f5.log" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'HEAD_TREE_UNREADABLE' "$TMP_DIR/gate-22f5.log" \
       && grep -q "still holds that commit's bytes" "$TMP_DIR/gate-22f5.log"; then
        t_pass "identity gate: the re-binding probe's own HEAD_TREE_UNREADABLE fires independently of the assembler's -- a second, distinct probe this group did not exercise before"
    else
        t_fail "identity gate: the re-binding probe's HEAD_TREE_UNREADABLE did not fire as expected (rc=$G_RC; see $TMP_DIR/gate-22f5.log)"
    fi
    # CONTROL: the same base/head pair, unshimmed, must pass cleanly -- proves
    # the failure above comes from the shim, not from stale fixture state.
    (cd "$GATE_REPO" && bash "$GATE_IN_CLONE" \
        --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22f5-control.log" 2>&1)
    G_CTL_RC=$?
    if [ "$G_CTL_RC" -eq 0 ] && grep -q '^\[identity-gate\] PASS' "$TMP_DIR/gate-22f5-control.log"; then
        t_pass "identity gate: the 22f5 control (same pair, no shim) passes cleanly, so the failure above measures the shim and not a broken fixture"
    else
        t_fail "identity gate: the 22f5 control did not pass on its own (rc=$G_CTL_RC; see $TMP_DIR/gate-22f5-control.log)"
    fi

    # ------------------------------------------------------------------
    # 22f6: HEAD_TRANSPORT_FAILED REACHED FROM A SOUND BASE (section 59).
    #
    # 22bw/22bx/22by/22bz already prove this gate's plumbing failures are
    # named rather than mis-blamed on the tree, but every one of them pairs
    # the head-transport failure with an already MALFORMED or REMOVED base
    # (`$G_CORRUPT`), so none of them shows the token firing when nothing else
    # is wrong. This shims only the SECOND non-`-d` `mktemp` call: base-first
    # ordering (identity-gate.sh:3989-3994) means the base's own transport
    # `mktemp` is always the first such call and the head's is always the
    # second, so a sound, unmodified base still resolves its own protocol
    # before the head's transport ever fails.
    G_SHIM_S59B="$TMP_DIR/gitshim-s59b-mktemp"
    mkdir -p "$G_SHIM_S59B"
    G_S59B_COUNT="$TMP_DIR/s59b-mktemp-calls"
    rm -f "$G_S59B_COUNT"
    {
        printf '#!/usr/bin/env bash\n'
        printf 'for a in "$@"; do [ "$a" = "-d" ] && exec %s "$@"; done\n' "$G_REAL_MKTEMP"
        printf 'printf x >> "%s"\n' "$G_S59B_COUNT"
        printf 'if [ "$(wc -c < "%s")" -ge 2 ]; then exit 7; fi\n' "$G_S59B_COUNT"
        printf 'exec %s "$@"\n' "$G_REAL_MKTEMP"
    } > "$G_SHIM_S59B/mktemp"
    chmod +x "$G_SHIM_S59B/mktemp"
    (cd "$GATE_REPO" && PATH="$G_SHIM_S59B:$PATH" bash "$GATE_IN_CLONE" \
        --base "$G_PROTO_BASE" --head HEAD >"$TMP_DIR/gate-22f6.log" 2>&1)
    G_RC=$?
    G_S59B_TOKEN="$(grep -oE 'INFRASTRUCTURE: [A-Z_]+' "$TMP_DIR/gate-22f6.log" | head -1)"
    if [ "$G_RC" -eq 3 ] \
       && grep -q 'HEAD_TRANSPORT_FAILED' "$TMP_DIR/gate-22f6.log" \
       && ! grep -qE 'BASE_[A-Z_]+' "$TMP_DIR/gate-22f6.log"; then
        t_pass "identity gate: HEAD_TRANSPORT_FAILED fires from a completely sound base, not only alongside an already-malformed one"
    else
        t_fail "identity gate: HEAD_TRANSPORT_FAILED did not fire cleanly from a sound base (rc=$G_RC; got [$G_S59B_TOKEN]; see $TMP_DIR/gate-22f6.log)"
    fi
    rm -f "$G_S59B_COUNT"

    # ------------------------------------------------------------------
    # 22f8: THE REFERENCE-CLOSURE PRUNE REFUSES AN INCOMPLETE CLOSURE (s59).
    #
    # `gate_reference_closure_prune` is what makes the pruned fixture tree safe
    # to compare against at all: it snapshots resolution over the FULL seeded
    # tree, prunes, snapshots again, and refuses unless all 134 mappings are
    # byte-identical. A comparison that can only ever agree with itself proves
    # nothing, so this case CRIPPLES the derivation and requires the refusal to
    # fire -- the same control discipline `22f4` uses for its scanner.
    #
    # THE LEG CHOSEN IS THE ONE THAT ACTUALLY MOVES VERDICTS.
    # `section_candidate_files` supplies the section-scope pairing evidence;
    # without it two symbols the FULL tree leaves `unpaired_ref` (several of
    # their section's files define them) RESOLVE in the pruned tree (only one
    # of those files survives), which is exactly the silent corpus change the
    # check exists to refuse. Dropping the `classify_ref` leg is deliberately
    # NOT used here: it changes nothing for today's six kept TODOs, which is
    # recorded at the derivation rather than dressed up as a second control.
    G_S59C_REPO="$TMP_DIR/gate-closure-control"
    G_S59C_ERR="$TMP_DIR/gate-22f8.err"
    if ! gate_seed_tree "$G_S59C_REPO" >/dev/null 2>&1; then
        t_fail "identity gate: could not seed the closure-control tree"
    else
        if ( cd "$G_S59C_REPO" \
                && export GATE_CLOSURE_DROP_LEG=section_candidates \
                && gate_reference_closure_prune ) >/dev/null 2>"$G_S59C_ERR"; then
            t_fail "identity gate: a crippled reference closure was ACCEPTED -- the before/after resolution check cannot see an incomplete closure (see $G_S59C_ERR)"
        elif grep -q 'reference closure is incomplete' "$G_S59C_ERR" \
             && grep -qE 'MOVED [1-9][0-9]* of [0-9]+ resolutions' "$G_S59C_ERR"; then
            t_pass "identity gate: an incomplete reference closure is refused, naming the resolutions it moved"
        else
            t_fail "identity gate: the crippled closure failed for the wrong reason, so the control proves nothing (see $G_S59C_ERR)"
        fi
        rm -rf "$G_S59C_REPO"
    fi

    # 22f9: THE PRUNE NEVER WRITES THROUGH A SYMLINK (section 59 round 2).
    #
    # The placeholder step used to truncate a non-closure `.c`/`.h` in place,
    # and `write_bytes` opens the LINK'S TARGET -- so a tracked `.c` symlink
    # under a bulk tree would have emptied whatever it pointed at, including an
    # absolute path on the host running the tooling suite. This plants exactly
    # that shape against a sentinel OUTSIDE the fixture clone and requires the
    # sentinel to survive, which is a property no assertion about the clone's
    # own contents can express.
    #
    # AND A DIRECTORY LINK NAMED `.c` IS THE SECOND ARM. `_basename_index`
    # indexes FILENAMES only, so a directory called `alias.c` is not in the
    # full tree's index; if the prune replaced it with an empty regular file
    # the pruned index would gain a basename the real one never had (Codex
    # adversarial, section 59 round 3, [medium]).
    #
    # THE CONTROL FAILS CLOSED ON ITS OWN CONSTRUCTION. `ln -s` returns 1 on an
    # existing path, and ignoring that let the case assert against an ordinary
    # tracked file that the prune had merely placeholdered -- every assertion
    # passing while nothing was ever linked, which is precisely the write-
    # through regression going undetected. Both links are therefore required to
    # be created AND verified as links at their exact targets before the prune
    # runs. The sentinel comparison is `cmp -s` against a pristine copy rather
    # than `[ "$(cat ...)" = ... ]`, because command substitution strips
    # trailing newlines and cannot be byte-exact (same finding).
    #
    # THE BASENAMES ARE DELIBERATELY UNIQUE. Adding a `.c` path changes
    # `_basename_index`, and a name colliding with an existing basename would
    # flip a bare-basename ref from resolved to ambiguous and make the prune's
    # own before/after check refuse for an unrelated reason -- the case would
    # then fail while proving nothing about symlinks.
    G_S59D_REPO="$TMP_DIR/gate-symlink-control"
    G_S59D_SENTINEL="$TMP_DIR/gate-22f9-sentinel.txt"
    G_S59D_EXPECT="$TMP_DIR/gate-22f9-expected.txt"
    printf 'SENTINEL MUST SURVIVE\n' > "$G_S59D_SENTINEL"
    cp "$G_S59D_SENTINEL" "$G_S59D_EXPECT"
    G_S59D_DIRTARGET="$TMP_DIR/gate-22f9-dirtarget"
    mkdir -p "$G_S59D_DIRTARGET"
    printf 'DIR TARGET MUST SURVIVE\n' > "$G_S59D_DIRTARGET/keep.txt"
    if ! gate_seed_tree "$G_S59D_REPO" >/dev/null 2>&1; then
        t_fail "identity gate: could not seed the symlink-control tree"
    else
        G_S59D_LINK="$G_S59D_REPO/src/kernel/zz_s59_symlink_control.c"
        G_S59D_DIRLINK="$G_S59D_REPO/src/kernel/zz_s59_dirlink_control.c"
        if ! ln -s "$G_S59D_SENTINEL" "$G_S59D_LINK" 2>/dev/null \
           || ! ln -s "$G_S59D_DIRTARGET" "$G_S59D_DIRLINK" 2>/dev/null \
           || [ ! -L "$G_S59D_LINK" ] || [ ! -L "$G_S59D_DIRLINK" ] \
           || [ "$(readlink "$G_S59D_LINK")" != "$G_S59D_SENTINEL" ] \
           || [ "$(readlink "$G_S59D_DIRLINK")" != "$G_S59D_DIRTARGET" ]; then
            t_fail "identity gate: could not PLANT the 22f9 symlink mutation, so the control would have proved nothing (checked before running the prune)"
        else
            ( cd "$G_S59D_REPO" && git add -A >/dev/null 2>&1 ) || true
            ( cd "$G_S59D_REPO" && gate_reference_closure_prune ) \
                >/dev/null 2>"$TMP_DIR/gate-22f9.err"
            G_S59D_RC=$?
            if ! cmp -s "$G_S59D_SENTINEL" "$G_S59D_EXPECT"; then
                t_fail "identity gate: the prune wrote THROUGH a symlink and altered a file outside the fixture ($G_S59D_SENTINEL)"
            elif [ ! -f "$G_S59D_DIRTARGET/keep.txt" ]; then
                t_fail "identity gate: the prune followed a DIRECTORY symlink out of the fixture and removed its contents"
            elif [ "$G_S59D_RC" -ne 0 ]; then
                t_fail "identity gate: the symlink control did not complete the prune (rc=$G_S59D_RC; see $TMP_DIR/gate-22f9.err)"
            elif [ -L "$G_S59D_LINK" ] || [ -L "$G_S59D_DIRLINK" ]; then
                t_fail "identity gate: a non-closure symlink survived the prune as a link, so a later checkout still carries it"
            elif [ -e "$G_S59D_DIRLINK" ]; then
                t_fail "identity gate: the pruned DIRECTORY link left something at $G_S59D_DIRLINK -- a `.c` placeholder there adds a basename the full tree's index never had"
            elif [ -f "$G_S59D_LINK" ] && [ ! -s "$G_S59D_LINK" ]; then
                t_pass "identity gate: a non-closure .c symlink becomes an empty REGULAR placeholder, a .c DIRECTORY link leaves nothing, and both targets are untouched"
            else
                t_fail "identity gate: the pruned file symlink left neither a link nor an empty regular file at $G_S59D_LINK"
            fi
        fi
        rm -rf "$G_S59D_REPO"
    fi

    # 22f7: THE GROUP'S RUNTIME STAYS UNDER A MEASURED CEILING (section 59).
    #
    # Baseline at section 53's ship: 408.75s over four runs, 438.5s with
    # section 53's eight fixtures (+30s/7.3%). That number is for the WHOLE
    # suite and predates sections 54, 56, 58 and this section's own additions,
    # so it is not reused as the bar. MEASURED at the section 59 ship, this
    # group alone (bracketed from `GATE_REPO=` above to here, so it excludes
    # every OTHER group in this 28,000-line file), over FOUR timed runs per
    # this section's own test checkpoint (Codex adversarial, section 59,
    # [medium] -- a single-run baseline cannot show a 2x margin is
    # non-flaky): 349.1s, 356.0s, 359.7s, 379.0s -- mean 361.0s, max 379.0s.
    # The ceiling below is 760s, roughly DOUBLE the observed MAX rather than
    # the mean: wide enough to survive the load-sensitivity already on record
    # for this suite (a 2s wall-clock sub-test elsewhere in this file has
    # flaked 2059-2349ms under load) while still catching the failure mode
    # this item names -- a later section silently doubling the group's cost
    # -- rather than tripping on a busy host.
    #
    # RE-MEASURED after this section's reference-closure prune and left at
    # 760s: 363.4s, 361.3s, 342.0s, 332.3s (mean 349.8s against the pre-prune
    # mean of 361.0s). The prune's saving is real per full-path invocation
    # (~1.15s of ~3.9s, measured directly on a full versus a pruned fixture
    # tree) but it is INSIDE this group's own run-to-run band, because section
    # 55 had already moved both full worktree checkouts behind the fast path
    # and most invocations never reach them. Tightening the bar on a mean that
    # moved less than the spread would buy flakes, not coverage.
    G_S59_GROUP_END_NS="$(mono_ns)"
    if ! G_S59_GROUP_MS=$(elapsed_ms "$G_S59_GROUP_START_NS" "$G_S59_GROUP_END_NS"); then
        t_fail "identity gate: could not measure the fixture group's own elapsed time (unreadable clock reading)"
    elif [ "$G_S59_GROUP_MS" -lt 760000 ]; then
        t_pass "identity gate: the fixture group finished in ${G_S59_GROUP_MS}ms, under its 760s ceiling"
    else
        t_fail "identity gate: the fixture group took ${G_S59_GROUP_MS}ms, at or over its 760s ceiling -- a later section grew the pre-push pack without anyone pricing it"
    fi

fi

# ----------------------------------------------------------------------
# ----------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------
tb_finish gate
