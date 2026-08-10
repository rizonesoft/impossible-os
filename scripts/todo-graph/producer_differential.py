#!/usr/bin/env python3
"""Differential the CACHE PRODUCER, which the resolver differential cannot see.

WHY THIS EXISTS (section 18, closing a section 16 blind spot).
`identity-gate.sh` builds the cache ONCE with head `build.py` and feeds that one
file to both resolver walks. Building it once is deliberate -- it is what makes
the occurrence keys stable, so a moved verdict is attributable to the resolver
change and nothing else. It is also a hole: if `build.py` stops emitting a
stamped ref, the ref is absent from BOTH walks, both sides agree perfectly, and
no DROPPED is ever produced. The identity differential is structurally incapable
of seeing a producer regression, and reporting PASS over a silently shrunken
population is the worst shape a gate can have.

So the producer gets its own differential, on the same principle applied one
level up: run the BASE and HEAD producers and compare what they emit.

TWO PROPERTIES MAKE IT HONEST.

1. ONE WORKTREE PER COMPARISON. Both producers run against the SAME corpus with
   the SAME `--repo-root`, so the git-derived `created_at` / `last_active_at`
   values are identical by construction and cannot manufacture a false failure.
   (The projection drops them anyway; belt and braces, because a differential
   that cries wolf gets disabled.)

2. BOTH CORPORA, NOT JUST HEAD'S. Over the head corpus alone the comparison
   proves equivalence only for syntax still PRESENT at head -- so a change that
   removes the last live example of a ref form AND stops parsing that form
   passes cleanly, then silently drops the form the day it is reintroduced.
   Running the pair over the base corpus as well is what closes that.

The projection is the STAMPED-REF POPULATION only: exactly the structure
`corpus_resolution_snapshot.collect()` iterates -- `stamped_items[*].refs` keyed
by the same `path#section.itemrN` occurrence identity. Title, status, ordering
and timestamps are all excluded, because a producer differential that fails on
prose churn is one nobody will keep.

Exit codes MIRROR corpus_resolution_snapshot.py deliberately -- a caller must be
able to read both tools the same way:
    0  the producers agree
    1  a REGRESSION: a stamped ref was dropped, or its payload changed
    2  usage error (this caller's bug)
    3  INFRASTRUCTURE: a producer failed, or emitted something unreadable.
       Distinct from 0/1 by design: "the gate could not run" is never "passed".
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path


class ProducerError(RuntimeError):
    """Infrastructure: a producer failed or emitted an unusable cache."""

    def __init__(self, message: str, stderr: str = "", returncode: int = 0):
        super().__init__(message)
        # CARRIED SO THE CALLER CAN ADJUDICATE, not for prettier output. The
        # novelty test below has to read the CATEGORY a refusal was emitted
        # under, and a message string that has already been tail-joined into
        # prose is not a parseable record of it.
        self.stderr = stderr
        self.returncode = returncode


# `[build.py] FAIL <file>: <category>: <message>` -- the shape build.py's own
# exit-code block documents. Anchored on the category being a lowercase-dashed
# token so the `read error: <exc>` line (which carries a SPACE, and is not a
# declared category) cannot be mistaken for one: an unparseable FAIL line makes
# the refusal not-provably-new, which fails closed.
_FAIL_LINE = re.compile(
    r"^\[build\.py\] FAIL (?P<file>[^:]+): (?P<cat>[a-z][a-z0-9-]*): ")


def _run_producer(tree: Path, corpus: Path, out: Path, label: str) -> None:
    """Run `tree`'s build.py over `corpus`, writing `out`.

    `--repo-root` and `--root` BOTH point into `corpus`, which is what pins the
    git-derived timestamps identical across the two producers.
    """
    script = tree / "scripts/todo-graph/build.py"
    if not script.is_file():
        raise ProducerError(f"{label}: no build.py at {script}")
    try:
        proc = subprocess.run(
                [sys.executable, str(script), "--quiet",
             "--root", str(corpus / "todo"),
             "--repo-root", str(corpus),
             "--output", str(out)],
            capture_output=True, text=True)
    except OSError as exc:
        raise ProducerError(f"{label}: cannot launch build.py: {exc}") from exc
    if proc.returncode != 0:
        raw = proc.stderr or proc.stdout or ""
        tail = raw.strip().splitlines()[-6:]
        raise ProducerError(
            f"{label}: build.py exited {proc.returncode} over {corpus}: "
            + " | ".join(tail), stderr=raw, returncode=proc.returncode)
    if not out.is_file() or out.stat().st_size == 0:
        raise ProducerError(f"{label}: build.py wrote no cache over {corpus}")


def declared_categories(tree: Path):
    """The set of error categories `tree`'s build.py declares it can emit.

    Returns a `set`, or None when the tree cannot answer -- which is a real and
    expected state, not a failure: every tree predating this probe has no
    `--emitted-error-categories` flag, and one of those is the BASE on every
    range until this commit becomes the last-gated SHA. None means "unaskable",
    and the caller must degrade rather than infer.

    Reads no corpus and writes no cache ON PURPOSE. It is only ever asked about
    a tree whose corpus the other producer could not process, so a probe that
    needed a readable corpus would be unanswerable exactly when it is needed.
    """
    script = tree / "scripts/todo-graph/build.py"
    if not script.is_file():
        return None
    try:
        proc = subprocess.run(
            [sys.executable, str(script), "--emitted-error-categories"],
            capture_output=True, text=True)
    except OSError:
        return None
    if proc.returncode != 0:
        return None
    try:
        got = json.loads(proc.stdout)
    except (ValueError, TypeError):
        return None
    # SHAPE-CHECKED, not merely parsed. A tree that answers with something other
    # than a list of non-empty strings has not answered this question, and
    # `set()` of a wrong shape would silently become an EMPTY declared set --
    # which would make every category look novel and approve every refusal.
    if not isinstance(got, list) or not got:
        return None
    if not all(isinstance(c, str) and c for c in got):
        return None
    return set(got)


def refusal_categories(stderr: str):
    """The fatal categories in a producer's stderr, or None if unparseable.

    None means at least one `FAIL` line did not carry a recognisable category
    -- a read error, a crash, a message shape this parser does not know. That
    must fail closed: a refusal whose reason cannot be read is never provably a
    new check.
    """
    cats, saw_fail = set(), False
    for line in stderr.splitlines():
        if not line.startswith("[build.py] FAIL "):
            continue
        # The trailing summary line (`FAIL: N fatal error(s) ...`) is a count,
        # not a per-file refusal; it has no category and must not read as one.
        if line.startswith("[build.py] FAIL: "):
            continue
        saw_fail = True
        m = _FAIL_LINE.match(line)
        if not m:
            return None
        cats.add(m.group("cat"))
    return cats if saw_fail else None


def classify_base_corpus_refusal(exc: ProducerError, base_tree: Path,
                                 head_tree: Path):
    """Why did the HEAD producer refuse the BASE corpus? Returns (verdict, why).

    verdict is one of:
      "new-check"  -- every category it refused under is one the base producer
                      provably could not emit. The base-corpus leg is unrunnable
                      BY CONSTRUCTION, not broken.
      "unproven"   -- the base tree cannot be asked, so novelty cannot be proven
                      either way.
      "genuine"    -- anything else. The refusal is a producer failure.

    THE SCOPE IS DELIBERATELY NARROW and every widening of it is a hole:
      * only the BASE corpus. A refusal of the HEAD corpus is the live tree
        failing to build and is never exempt.
      * only the HEAD producer. A BASE-producer refusal of the base corpus means
        the corpus is broken independently of the change under test.
      * only when EVERY category is provably new. One familiar category among
        several new ones is a producer regression wearing a new check as cover.
    """
    cats = refusal_categories(exc.stderr)
    if cats is None:
        return ("genuine", "the refusal carries no readable error category, so "
                           "nothing attributes it to a new check")
    head_set = declared_categories(head_tree)
    if head_set is None:
        return ("genuine", "the HEAD tree does not declare its emitted error "
                           "categories, so novelty cannot be established")
    undeclared = sorted(cats - head_set)
    if undeclared:
        # The head producer emitted a category it does not declare. Its own
        # runtime binding should have made this impossible, so reaching here
        # means the declaration is not trustworthy -- and an untrustworthy
        # declaration must not be the thing that waves a refusal through.
        return ("genuine", f"the HEAD producer refused under undeclared "
                           f"category/categories {undeclared}, so its "
                           f"declaration does not describe what it emits")
    base_set = declared_categories(base_tree)
    if base_set is None:
        return ("unproven", "the BASE tree predates --emitted-error-categories, "
                            "so it cannot say whether these checks existed")
    familiar = sorted(cats & base_set)
    if familiar:
        return ("genuine", f"category/categories {familiar} existed at BASE "
                           f"too, so this refusal is not attributable to a new "
                           f"check")
    return ("new-check", f"category/categories {sorted(cats)} exist only at "
                         f"HEAD, so the base corpus predates the repair they "
                         f"require")


def project(cache_path: Path, label: str) -> dict:
    """Reduce a cache to its stamped-ref population.

    The value carries `kind` as well as `file`/`symbol` on purpose. The resolver
    walk only ever looks at `kind == "symbol"` refs, so a producer regression
    that FLIPS a ref's kind removes it from the walk exactly as completely as
    dropping it -- and would be invisible to a projection that filtered on kind
    before comparing.
    """
    try:
        nodes = json.loads(cache_path.read_text(encoding="utf-8"))
    except (OSError, ValueError, RecursionError) as exc:
        raise ProducerError(f"{label}: cache unreadable: {exc}") from exc
    if not isinstance(nodes, list) or not nodes:
        raise ProducerError(f"{label}: cache is not a non-empty list")

    out = {}
    for node in nodes:
        if not isinstance(node, dict):
            raise ProducerError(f"{label}: cache holds a non-object node")
        todo_path = node.get("file_path") or "?"
        for it in node.get("stamped_items") or []:
            sec = it.get("section_n", "?")
            idx = it.get("item_idx", "?")
            for ref_i, ref in enumerate(it.get("refs") or []):
                key = f"{todo_path}#{sec}.{idx}r{ref_i}"
                # A COLLISION IS A HARD ERROR, never a silent overwrite -- the
                # same rule `collect()` enforces on the same identity, and for
                # the same reason: `out[key] = ...` would drop one occurrence
                # while the counts still recorded both, leaving a projection
                # that is SELF-CONSISTENT and quietly smaller than the corpus.
                # It would then compare clean forever (Codex design review,
                # section 18).
                if key in out:
                    raise ProducerError(
                        f"{label}: duplicate projection key {key!r} -- two "
                        f"refs share one identity, so this differential would "
                        f"silently compare fewer refs than the corpus holds")
                out[key] = {
                    "kind": ref.get("kind"),
                    "file": ref.get("file"),
                    "symbol": ref.get("symbol"),
                }
    if not out:
        raise ProducerError(
            f"{label}: the projection is EMPTY -- refusing to treat a corpus "
            f"with no stamped refs as a result, which would pass vacuously")
    return out


def diff_one(base_proj: dict, head_proj: dict, corpus_label: str,
             strict: bool) -> int:
    """Report one corpus's producer diff. Returns the count of FAILING rows."""
    dropped = sorted(k for k in base_proj if k not in head_proj)
    added = sorted(k for k in head_proj if k not in base_proj)
    changed = sorted(k for k in base_proj
                     if k in head_proj and base_proj[k] != head_proj[k])

    print(f"[{corpus_label}] base producer: {len(base_proj)} stamped refs; "
          f"head producer: {len(head_proj)}")
    for k in dropped:
        print(f"  DROPPED {k} was {base_proj[k]}")
    for k in changed:
        print(f"  CHANGED {k} {base_proj[k]} -> {head_proj[k]}")
    for k in added:
        print(f"  ADDED   {k} -> {head_proj[k]}  (GROUND-TRUTH BY HAND)")

    fails = len(dropped) + len(changed)
    if strict:
        # Same reasoning as the snapshot tool's --strict: an ADDED ref cannot
        # hide a loss, so it is advisory for a HUMAN -- but nothing here has
        # ground-truthed it, and a parser that starts binding the WRONG thing
        # looks exactly like a coverage win to an automated caller reading only
        # the exit code.
        fails += len(added)
    return fails


def main(argv) -> int:
    strict = "--strict" in argv
    argv = [a for a in argv if a != "--strict"]
    if len(argv) != 2:
        sys.stderr.write(
            "usage: producer_differential.py <base-tree> <head-tree> "
            "[--strict]\n"
            "  Runs each tree's build.py over BOTH trees' corpora and compares\n"
            "  the stamped-ref population the resolver walk consumes.\n")
        return 2

    base_tree, head_tree = Path(argv[0]).resolve(), Path(argv[1]).resolve()
    for t, lbl in ((base_tree, "base"), (head_tree, "head")):
        if not (t / "todo").is_dir():
            sys.stderr.write(f"[producer-differential] {lbl} tree has no "
                             f"todo/ directory: {t}\n")
            return 3

    total_fails = 0
    legs_run = []
    legs_skipped = []
    try:
        with tempfile.TemporaryDirectory(prefix="producer-diff.") as tmp:
            tmpd = Path(tmp)
            # BOTH CORPORA. The head corpus alone cannot see a regression whose
            # only live example was deleted in the same change.
            for corpus, corpus_label in ((base_tree, "base-corpus"),
                                         (head_tree, "head-corpus")):
                b_out = tmpd / f"{corpus_label}-by-base.json"
                h_out = tmpd / f"{corpus_label}-by-head.json"
                # BASE PRODUCER FIRST, and the order is load-bearing. Reaching
                # the head run at all therefore proves the base producer
                # ACCEPTED this corpus, which is half of what makes a head-side
                # refusal below attributable to the head change rather than to a
                # corpus that was already broken.
                _run_producer(base_tree, corpus, b_out,
                              f"base producer over {corpus_label}")
                try:
                    _run_producer(head_tree, corpus, h_out,
                                  f"head producer over {corpus_label}")
                except ProducerError as exc:
                    # THE NEW-CHECK EXEMPTION (section 39), scoped to exactly
                    # one leg. A commit that adds a fatal check ships the corpus
                    # repair that check demands -- it has to, or the producer
                    # would fail on the live tree -- so the new check is
                    # UNRUNNABLE over any corpus predating that repair. That is
                    # a property of the range, not a defect in the producer, and
                    # rc 3 on it wedges the gate permanently because the last-
                    # gated SHA only advances on a PASS.
                    if corpus_label != "base-corpus":
                        raise
                    verdict, why = classify_base_corpus_refusal(
                        exc, base_tree, head_tree)
                    if verdict == "genuine":
                        raise
                    banner = ("NEW CHECK" if verdict == "new-check"
                              else "UNPROVEN")
                    print(f"\n[base-corpus] SKIPPED ({banner}): the head "
                          f"producer refused the base corpus -- {why}.")
                    print(f"[base-corpus]   {exc}")
                    print("[base-corpus] This leg proves nothing on this "
                          "range. The head-corpus leg below is NOT relaxed, "
                          "and a refusal of the HEAD corpus is never exempt.")
                    if verdict == "unproven":
                        print("[base-corpus] NOTE: novelty could not be "
                              "PROVEN -- the base tree predates the "
                              "--emitted-error-categories probe. Once a base "
                              "carrying it becomes the last-gated SHA, this "
                              "degrades to a proof instead of an assumption.")
                    legs_skipped.append((corpus_label, verdict))
                    continue
                total_fails += diff_one(
                    project(b_out, f"base producer over {corpus_label}"),
                    project(h_out, f"head producer over {corpus_label}"),
                    corpus_label, strict)
                legs_run.append(corpus_label)
    except ProducerError as exc:
        sys.stderr.write(f"[producer-differential] {exc}\n")
        return 3
    except OSError as exc:
        # OPERATIONAL FAILURES ARE INFRASTRUCTURE, not a regression verdict. An
        # unusable TMPDIR raised straight out of TemporaryDirectory as a
        # traceback and a bare exit 1 -- and 1 is the DOCUMENTED "a stamped ref
        # was dropped" code, so identity-gate.sh would have reported a producer
        # regression that never happened (Codex consistency, section 18
        # review). Same contract the snapshot tool applies to its own I/O.
        sys.stderr.write(f"[producer-differential] operational failure: "
                         f"{exc}\n")
        return 3

    if total_fails:
        print(f"\nFAIL: {total_fails} producer difference(s) across both "
              f"corpora. A stamped ref the resolver walk consumes was dropped, "
              f"changed, or added unreviewed -- the identity differential "
              f"CANNOT see this, because a ref missing from the one shared "
              f"cache is missing from both of its walks.")
        return 1
    # A PASS WITH NO LEG RUN IS NOT A PASS. Every skip above is bounded to the
    # base corpus, so the head-corpus leg must always have run -- but asserting
    # it here is what stops a later widening from quietly turning this tool into
    # one that returns 0 having compared nothing.
    if "head-corpus" not in legs_run:
        sys.stderr.write("[producer-differential] the head-corpus leg did not "
                         "run, so nothing was compared. Refusing to report a "
                         "pass.\n")
        return 3
    if legs_skipped:
        skipped = ", ".join(f"{lbl} ({v})" for lbl, v in legs_skipped)
        print(f"\nOK (DEGRADED): both producers emit an identical stamped-ref "
              f"population over the head corpus. Legs skipped: {skipped}.")
        return 0
    print("\nOK: both producers emit an identical stamped-ref population over "
          "both corpora.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
