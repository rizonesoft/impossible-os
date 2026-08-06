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
import subprocess
import sys
import tempfile
from pathlib import Path


class ProducerError(RuntimeError):
    """Infrastructure: a producer failed or emitted an unusable cache."""


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
        tail = (proc.stderr or proc.stdout or "").strip().splitlines()[-6:]
        raise ProducerError(
            f"{label}: build.py exited {proc.returncode} over {corpus}: "
            + " | ".join(tail))
    if not out.is_file() or out.stat().st_size == 0:
        raise ProducerError(f"{label}: build.py wrote no cache over {corpus}")


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
    try:
        with tempfile.TemporaryDirectory(prefix="producer-diff.") as tmp:
            tmpd = Path(tmp)
            # BOTH CORPORA. The head corpus alone cannot see a regression whose
            # only live example was deleted in the same change.
            for corpus, corpus_label in ((base_tree, "base-corpus"),
                                         (head_tree, "head-corpus")):
                b_out = tmpd / f"{corpus_label}-by-base.json"
                h_out = tmpd / f"{corpus_label}-by-head.json"
                _run_producer(base_tree, corpus, b_out,
                              f"base producer over {corpus_label}")
                _run_producer(head_tree, corpus, h_out,
                              f"head producer over {corpus_label}")
                total_fails += diff_one(
                    project(b_out, f"base producer over {corpus_label}"),
                    project(h_out, f"head producer over {corpus_label}"),
                    corpus_label, strict)
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
    print("\nOK: both producers emit an identical stamped-ref population over "
          "both corpora.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
