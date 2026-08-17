#!/usr/bin/env python3
"""Deterministic section manifest -- Opus starts oriented, not exploring.

Generated BEFORE the implementing session touches a section (and fed as the
seed of the Sonnet enrichment dispatch): exact section text coordinates, open
items, XREFs, likely files, relevant tests, required gates, and current blob
hashes -- everything derivable without a model. The Sonnet mapper
(section-context-mapper / kernel-explorer) enriches this into the behavioral
package; Opus verifies and decides.

Usage: section-manifest.py <todo-path> <section-n> [--project DIR]
Output: JSON manifest on stdout.
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from worktree_hash import content_hashes  # noqa: E402  (sibling helper)
from section_slice import SECTION_RE, section_block  # noqa: E402  (sibling helper)
ITEM_RE = re.compile(r"^- \[([ x/])\]\s*(.*)")
XREF_RE = re.compile(r"XREF:\s*`?([^`\n]+)`?")
FILE_RE = re.compile(
    r"\b((?:src|include|user|tools|scripts)/[A-Za-z0-9_/.-]+\.(?:c|h|asm|S|sh|py|ld))\b")
# Boot-path surfaces that make the smoke test mandatory (CLAUDE.md list).
BOOT_PATHS = ("src/boot/", "src/kernel/main/boot_", "src/kernel/idt.c",
              "src/kernel/gdt.c", "src/kernel/msr.c", "src/kernel/smp/",
              "src/kernel/mm/pmm.c", "src/kernel/mm/vmm.c",
              "src/kernel/mm/heap.c", "src/kernel/drivers/lapic.c",
              "src/kernel/drivers/ioapic.c", "src/kernel/drivers/acpi.c",
              "src/kernel/drivers/timer.c")


def _git(root, *args):
    try:
        r = subprocess.run(["git", "-C", str(root), *args],
                           capture_output=True, text=True, timeout=30)
        return r.stdout if r.returncode == 0 else ""
    except Exception:
        return ""


# section_block now lives in section_slice.py so this file and
# section-pack.py cannot drift into an oracle split-brain. The private copy
# terminated only on the next NUMBERED heading, so the LAST `## N.` section
# of every file sliced to EOF and swallowed the file-level OS Comparison /
# Unit Tests / Verification / History blocks -- inflating open_items and
# emitting false SPLIT-RECOMMENDED verdicts (bare-metal-hardening's terminal
# section reported 21 items against a real 4; kernel-resource-accounting-
# quotas' terminal section reported 95 against a real 5).


_WAIVER_FIELDS = {
    "est_files": int,       # files the section will actually touch
    "subsystems": list,     # top-2-dir subsystems it spans
    "est_tests": int,       # unit tests it will add/modify
    "context_budget": str,  # e.g. "150K" -- estimated context it fits in
    "rationale": str,       # why it is cohesive despite the SPLIT flag
}


# ---------------------------------------------------------------------------
# SPAWN-CHAIN SENSOR (2026-08-05). The split predictor above scores a section's
# SIZE. It has nothing to say about a section's ORIGIN, and origin is where the
# expensive failure lives: TODO-07 ran sections 20->26, seven consecutive links,
# each created from the PREVIOUS section's review finding another edge case in
# the same teardown path. Branching factor 1.0 for days. Every counter the
# runner has read healthy throughout -- sections shipped, reviews passed, the IO
# table filled -- because none of them can see that section N exists only
# because of section N-1.
#
# COMPLETION-FIRST IS NOT WEAKENED BY THIS AND MUST NOT BE. A discovered gap is
# still ALWAYS filed. This sensor never decides WHETHER to record work, only
# WHERE: a new section, or a parked `- [/]` item in the parent naming its
# blocker. Both keep the work visible; only one grows the file.
#
# The load-bearing distinction is WHY a section was created:
#   (split)  -- decomposition at AUTHORING time, before implementation. Healthy:
#               the same work, correctly partitioned. TODO-06 sections 10/11 were
#               split because a parse fix and a lint-reporting change carry
#               opposite failure modes. Does NOT extend a chain.
#   (review) -- created from the PREVIOUS section's review. This is the recursion
#               shape, and consecutive review-spawns are what the chain counts.
#   (root)   -- nothing spawned it: a capability that stands on its own. Added
#               2026-08-09 so provenance can be REQUIRED without forcing a
#               genuinely new section to invent a parent. It contributes 0 depth
#               exactly like an unmarked section, so it changes no verdict; what
#               it buys is that "this is not a spawn" becomes a CLAIM someone
#               made rather than the default you get by saying nothing. The
#               sensor was blind to 2,390 of 2,410 sections for want of it.
_SPAWN_RE = re.compile(
    r"^>\s*\*\*Spawned-by:\*\*\s*(?:(root)\b|(?:§|section\s*)(\d+)\s*\((split|review)\))",
    re.I | re.M)

# Depth at which a further review-spawn needs an accountable justification.
# THREE, not two: a section reviewing into one successor is ordinary
# completion-first, and two is a run of bad luck. Three consecutive links on one
# surface is a pattern, and TODO-07 reached SEVEN before anything noticed.
SPAWN_CHAIN_LIMIT = 3

_CONTINUATION_FIELDS = {
    "user_impact": str,     # what a user hits if this is NOT done
    "not_parkable": str,    # why it cannot be a `- [/]` park in the parent
    "severity_trend": str,  # this round's finding severities vs the last
    "surface": str,         # the file/subsystem it touches
}


def validate_continuation_waiver(waiver: object) -> tuple:
    """A review-spawn at or past SPAWN_CHAIN_LIMIT needs a STRUCTURED waiver,
    exactly as a SPLIT-RECOMMENDED override does -- and for the same reason: the
    override must be an accountable prediction, not the word "needed".

    `user_impact` and `not_parkable` are the two that actually bite. A chain link
    that cannot name what a user hits, or cannot say why a park in the parent
    would lose something, is refinement below the depth the component warrants --
    which is the TODO-07 shape. Returns (ok, missing)."""
    if not isinstance(waiver, dict):
        return (False, ["<waiver must be a JSON object with "
                        + ", ".join(_CONTINUATION_FIELDS) + ">"])
    missing = []
    for key, typ in _CONTINUATION_FIELDS.items():
        val = waiver.get(key)
        ok = isinstance(val, typ) and not isinstance(val, bool)
        if ok and isinstance(val, str) and not val.strip():
            ok = False
        if not ok:
            missing.append(key)
    return (not missing, missing)


def spawn_chains(lines: list) -> dict:
    """Map section number -> (parent, kind, review_chain_depth).

    Depth counts `(review)` links walking up the chain. A `(split)` adds no
    depth of its own -- decomposition is not recursion -- but it PROPAGATES the
    depth it inherits, which is the whole correction here.

    WHY (measured 2026-08-09 on the todo-metadata-layer TODO, which grew from 9
    to 35 sections in seven days). A split used to RESET the count to 0, so the
    sensor never saw a chain deeper than 2 against a limit of 3: it had never
    once fired, and the cascade ran underneath it. The observed shape was a
    review-spawn, then a split of that successor into three, each restarting at
    0, then another review-spawn off one of those, and so on. Eleven splits
    interleaved with nine review-spawns kept every chain permanently under the
    limit.

    Splitting a review-spawned section does not make its successors less
    recursive -- it multiplies the surface that can spawn the next one. So a
    split now carries its parent's depth forward unchanged, the next
    review-spawn is one deeper, and the one after that has to answer the
    continuation waiver instead of quietly becoming another section.

    A section with no marker is a root, so every pre-existing section is
    unaffected -- this sensor is additive and silent until sections declare
    provenance.
    """
    parents = {}
    cur = None
    for ln in lines:
        m = re.match(r"^## (\d+)\.", ln)
        if m:
            cur = int(m.group(1))
            continue
        if cur is None:
            continue
        sm = _SPAWN_RE.match(ln)
        if sm:
            if sm.group(1):          # `(root)`: declared parentless
                continue             # same as unmarked -- a root, depth 0
            parents[cur] = (int(sm.group(2)), sm.group(3).lower())

    def _depth(n, seen):
        # cycle-safe: a malformed self- or mutual-reference stops at 0 rather
        # than recursing forever. A sensor must never be the thing that hangs.
        if n in seen or n not in parents:
            return 0
        par, kind = parents[n]
        if kind != "review":
            # A split CARRIES the chain it was cut from. Returning 0 here is
            # what let a decomposition launder a recursive chain back to zero.
            return _depth(par, seen | {n})
        return 1 + _depth(par, seen | {n})

    return {n: (parents[n][0], parents[n][1], _depth(n, set()))
            for n in parents}


# CAP HEADROOM. The section cap is a fixed BUDGET, and splitting spends it:
# measured 2026-08-10 on the todo-metadata-layer TODO, 13 of its 39 sections are
# split CHILDREN -- a third of the file's cap consumption is re-partitioning work
# that already existed, and every slot spent that way is a slot unavailable to a
# review finding that genuinely needs a home.
#
# The predictor is blind to this by construction. Its calibration optimised turns
# PER SECTION and priced a split at ~60 turns of fixed review overhead; it never
# modelled that a section also consumes a cap slot AND brings its own
# adversarial/consistency/perf wave, which is the surface that generates the next
# finding. The local metric is well-tuned, the global one is not in the model.
#
# So the bar rises as a file approaches its cap -- precisely when a slot is worth
# more spent on a finding than on decomposition. Below the window nothing
# changes, because a file with room to grow should decompose freely.
#
# The soft cap is IMPORTED, never restated: `scripts/todo-staged-check.py` owns
# that number and a second copy here would drift the day one of them moves. If
# the import fails the headroom is UNKNOWN and the ordinary threshold applies --
# fail-soft to today's behaviour rather than to a guessed constant.
# 20, chosen so the tight zone starts at 30 sections with the soft cap at 50.
# MEASURED against the todo-metadata-layer TODO's own 13 split children: a
# trigger at 30 would have applied the higher bar to 5 of them (the two recent
# 2-way splits plus one earlier), 25 is indistinguishable from 30 on this data,
# and 35 catches only 2. A trigger at 20 catches 9 -- but it would also have hit
# the file's most defensible decomposition, a genuinely oversized section split
# three ways early on. Applying the hardest test to the best split is the wrong
# trade, so the window stops short of it.
#
# 30 is also where that file's growth actually turned: it sat at 9 sections for
# weeks, then went 23 -> 26 -> 33 -> 39 in four days.
HEADROOM_WINDOW = 20          # sections from the soft cap where the bar rises
ITEM_THRESHOLD = 5            # normal
ITEM_THRESHOLD_TIGHT = 8      # inside the window


def _soft_cap():
    """The section soft cap, from its owner. None when it cannot be read."""
    try:
        import importlib.util
        src = Path(__file__).resolve().parents[1] / "todo-staged-check.py"
        spec = importlib.util.spec_from_file_location("_tsc", str(src))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        cap = getattr(mod, "SECTION_SOFT_CAP", None)
        return cap if isinstance(cap, int) else None
    except Exception:
        return None


def section_total(lines: list) -> int:
    """Sections in the file, counted with the SHARED heading rule."""
    return sum(1 for ln in lines if SECTION_RE.match(ln))


def item_threshold(total_sections: int) -> tuple:
    """(threshold, headroom, tight). `headroom` is None when the cap is unknown."""
    cap = _soft_cap()
    if cap is None:
        return (ITEM_THRESHOLD, None, False)
    headroom = cap - total_sections
    tight = headroom <= HEADROOM_WINDOW
    return (ITEM_THRESHOLD_TIGHT if tight else ITEM_THRESHOLD, headroom, tight)


# When headroom is SHORT the waiver must also answer the budget question. Every
# section is a cap slot, so overriding a split near the cap is not just "this is
# cohesive" -- it is "this decomposition is worth a slot that a pending review
# finding will then not have". That is a different claim and it deserves saying
# out loud. Required ONLY inside the window; below it a split is cheap and the
# ordinary five fields stand.
_WAIVER_TIGHT_FIELD = "cap_tradeoff"


def validate_split_waiver(waiver: object, tight: bool = False) -> tuple:
    """P3.1: a SPLIT-RECOMMENDED verdict may be overridden ONLY by a STRUCTURED
    waiver, never a free-form "cohesive". Requires a concrete ESTIMATE for each
    dimension the split predictor scores, so overriding the split is an
    accountable prediction rather than an assertion. Returns (ok, missing)."""
    if not isinstance(waiver, dict):
        return (False, ["<waiver must be a JSON object with "
                        + ", ".join(_WAIVER_FIELDS) + ">"])
    missing = []
    fields = dict(_WAIVER_FIELDS)
    if tight:
        fields[_WAIVER_TIGHT_FIELD] = str
    for key, typ in fields.items():
        val = waiver.get(key)
        ok = isinstance(val, typ) and not isinstance(val, bool)
        if ok and isinstance(val, str) and not val.strip():
            ok = False
        if ok and isinstance(val, list) and not val:
            ok = False
        if not ok:
            missing.append(key)
    return (not missing, missing)


def main(argv) -> int:
    if len(argv) >= 1 and argv[0] == "waiver-check":
        # section-manifest.py waiver-check <waiver.json>  -- exit 0 iff structured.
        if len(argv) < 2:
            print("usage: section-manifest.py waiver-check <waiver.json>",
                  file=sys.stderr)
            return 2
        try:
            waiver = json.loads(Path(argv[1]).read_text(encoding="utf-8"))
        except Exception as exc:
            print(json.dumps({"ok": False, "error": f"unreadable waiver: {exc}"}))
            return 1
        _tight = "--tight" in argv
        ok, missing = validate_split_waiver(waiver, tight=_tight)
        print(json.dumps({"ok": ok, "missing": missing, "tight": _tight,
                          "note": ("structured waiver accepted" if ok else
                                   "SPLIT-RECOMMENDED override needs a structured "
                                   "waiver; a free-form 'cohesive' is not enough"
                                   + (" -- and near the cap it must also say why "
                                      "this decomposition earns a slot a pending "
                                      "finding will not get (cap_tradeoff)"
                                      if _tight else ""))}))
        return 0 if ok else 1
    if len(argv) >= 1 and argv[0] == "spawn-chain":
        # section-manifest.py spawn-chain <todo-path>
        # Reports review-spawn chain depth per section. Exit 1 if any link is at
        # or past the limit, so it can gate; exit 0 otherwise.
        if len(argv) < 2:
            print("usage: section-manifest.py spawn-chain <todo-path>",
                  file=sys.stderr)
            return 2
        try:
            lines = Path(argv[1]).read_text(encoding="utf-8").split("\n")
        except Exception as exc:
            print(json.dumps({"ok": False, "error": f"unreadable: {exc}"}))
            return 2
        chains = spawn_chains(lines)
        deep = {n: v for n, v in chains.items()
                if v[1] == "review" and v[2] >= SPAWN_CHAIN_LIMIT}
        print(json.dumps({
            "limit": SPAWN_CHAIN_LIMIT,
            "sections": {str(n): {"parent": v[0], "kind": v[1], "depth": v[2]}
                         for n, v in sorted(chains.items())},
            "over_limit": sorted(deep),
            "note": ("a review-spawn at or past the limit needs a structured "
                     "continuation waiver, or the finding is PARKED in the "
                     "parent as `- [/]` -- filed either way, never dropped"),
        }, indent=1))
        return 1 if deep else 0
    if len(argv) >= 1 and argv[0] == "continuation-check":
        if len(argv) < 2:
            print("usage: section-manifest.py continuation-check <waiver.json>",
                  file=sys.stderr)
            return 2
        try:
            waiver = json.loads(Path(argv[1]).read_text(encoding="utf-8"))
        except Exception as exc:
            print(json.dumps({"ok": False, "error": f"unreadable waiver: {exc}"}))
            return 1
        ok, missing = validate_continuation_waiver(waiver)
        print(json.dumps({"ok": ok, "missing": missing,
                          "note": ("continuation accepted" if ok else
                                   "a deep review-spawn needs user_impact + "
                                   "not_parkable; 'needed' is not enough")}))
        return 0 if ok else 1
    if len(argv) < 2:
        print("usage: section-manifest.py <todo-path> <section-n> [--project DIR]",
              file=sys.stderr)
        return 2
    todo_rel, n = argv[0], int(argv[1])
    root = Path(argv[argv.index("--project") + 1]).resolve() \
        if "--project" in argv else Path(".").resolve()
    todo = root / todo_rel
    try:
        text = todo.read_text(encoding="utf-8")
    except OSError as exc:
        print(json.dumps({"error": f"unreadable TODO: {exc}"}))
        return 1
    block = section_block(text, n)
    if not block:
        print(json.dumps({"error": f"section {n} not found in {todo_rel}"}))
        return 1

    heading = block.splitlines()[0]
    open_items, done_items = [], 0
    for ln in block.splitlines():
        mm = ITEM_RE.match(ln.strip())
        if not mm:
            continue
        if mm.group(1) == "x":
            done_items += 1
        else:
            open_items.append({"status": mm.group(1),
                               "text": mm.group(2)[:220]})

    xrefs = sorted({x.strip()[:160] for x in XREF_RE.findall(block)})
    likely_files = sorted({f for f in FILE_RE.findall(block)
                           if (root / f).exists()})
    # Also surface preamble Inputs paths (shared integration surface).
    preamble = text.split("## 1.", 1)[0]
    input_files = sorted({f for f in FILE_RE.findall(preamble)
                          if (root / f).exists() and f not in likely_files})

    # Relevant tests: any test file sharing a basename token with a likely file.
    tokens = {Path(f).stem for f in likely_files if f.startswith(("src/", "include/"))}
    tests = []
    for tf in sorted((root / "src/kernel/test").glob("test_*.c")):
        stem = tf.stem[5:]  # drop test_
        if any(stem in t or t in stem for t in tokens if len(t) > 3):
            tests.append(str(tf.relative_to(root)))

    kernelish = any(f.startswith(("src/kernel/", "include/kernel/"))
                    for f in likely_files)
    bootish = any(f.startswith("src/boot/") or
                  any(f.startswith(bp) for bp in BOOT_PATHS)
                  for f in likely_files)
    gates = ["codex design review (pre-edit)",
             "codex adversarial-impl + fix loop",
             "build (=== BUILD OK ===) + test.sh green",
             "review-todo-section: adversarial + consistency + perf "
             "(+ re-adversarial on triggers)",
             "receiving-code-review on every findings set",
             "section commit gate (content-bound receipts)"]
    if kernelish:
        gates.insert(0, "kernel-code-quality gates (auto)")
        gates.append("kernel-quality-auditor (review step 7)")
    if bootish:
        gates.insert(0, "boot-code-quality gates (auto)")
        gates.append("smoke test (boot-path change)")

    # Content hashes: TODO + likely files. WORKING-TREE bytes (untracked +
    # unstaged included), NOT git-index blobs -- a mid-implementation session
    # has unstaged/untracked changes, and the run executes the live tree, so
    # index-blob binding would reuse stale evidence (2026-07-11 fix).
    blobs = content_hashes(root, [todo_rel, *likely_files])

    # Complexity budget (authoring-time split signal): a section likely to
    # exceed one fresh worker context should be SPLIT before implementation;
    # the quality pipeline runs per resulting section either way.
    subsystems = sorted({"/".join(f.split("/")[:2]) for f in likely_files})
    abi_impact = bool(re.search(
        r"(?i)\b(ABI|NTSTATUS|SSDT|boot_info|struct offset|BOOT_INFO_VERSION"
        r"|syscall number|PEB|TEB)\b", block))
    split_reasons = []
    if len(likely_files) > 8:
        split_reasons.append(f"{len(likely_files)} files")
    # CALIBRATED 2026-07-28 against 15 shipped sections reconstructed from
    # history (pre-ship manifest features paired with the turns each actually
    # cost, via metrics start_sha..end_sha ranges).
    #
    # `open_items` is the ONLY feature with predictive power: r = +0.50 against
    # turns. files (+0.16), subsystems (+0.05) and abi_impact (-0.22) have
    # none, and the old composite verdict scored +0.20 -- near-random. It
    # flagged 2 of the 8 sections that ran past 250 turns (25% recall) while
    # firing on one that took 248.
    #
    # The threshold is chosen to MINIMISE TOTAL COST, not to maximise recall,
    # because splitting is not free: a section carries ~60 turns of fixed
    # review overhead (the floor observed in zero-item sections, 58 and 85
    # turns), so splitting below T ~= 220 costs MORE than it saves. Modelled
    # over the 15-section set: `> 12` captured +2.5%, `>= 6` +6.5%, `>= 5`
    # +7.0%, `>= 4` +9.5%, against a PERFECT oracle's +11.7% ceiling.
    #
    # `>= 5` is the balance point -- nearly triple the old capture, splitting
    # 9 of 15 rather than 11 or 12. Two long sections (352 and 379 turns) have
    # only 3-4 items and are invisible to ANY item-count rule; catching those
    # needs a different mechanism, not a lower number here.
    # WORK items, not bookkeeping. Every section carries a `Commit: "..."` item
    # (1,464 of them corpus-wide) that costs no implementation turns, and it was
    # being counted toward the split verdict -- so a section with FOUR real
    # items tripped a threshold calibrated on effort. Every split waiver written
    # against this argued the same point by hand ("one of the five is the commit
    # line ... the section carries THREE work items"); when the override is
    # always the same paragraph, the rule is wrong, not the sections.
    #
    # THE TRADE, stated rather than hidden: the calibration above measured
    # `open_items` INCLUDING the commit line, so dropping it and keeping `>= 5`
    # raises the effective bar by one and moves the modelled capture from +7.0%
    # to +6.5% -- half a point. That is worth it because a split is not free
    # (~60 turns of fixed review overhead per section), so one avoided
    # unnecessary split repays the difference many times. Recalibrating the
    # number properly needs a fresh turns-vs-items sample; until then this is a
    # deliberate half-point, not an oversight.
    #
    # Only the commit line is excluded. `Unit tests:` / `Tests:` items look like
    # boilerplate and are not -- they carry real assertion lists -- so they keep
    # counting.
    work_items = [it for it in open_items
                  if not re.match(r"(?i)\s*commit\b\s*[:\-]?", it["text"])]
    _thresh, _headroom, _tight = item_threshold(section_total(text.split("\n")))
    if len(work_items) >= _thresh:
        split_reasons.append(
            f"{len(work_items)} work items"
            + (f" (>= {_thresh}; {_headroom} sections of cap headroom left)"
               if _tight else ""))
    if len(subsystems) > 3:
        split_reasons.append(f"{len(subsystems)} subsystems")
    # REMOVED 2026-08-17: the ABI-weighted item arm. It was added by P3.1
    # (2026-07-14) when the general item gate was `> 8`, so `abi_impact and
    # items >= 6` genuinely caught sections the general gate missed. The
    # calibration two weeks later (2026-07-28) dropped the general gate to
    # `>= 5` loose / `>= 8` tight, at which point the ABI arm became
    # ARITHMETICALLY DOMINATED: it fired at `>= 6` loose and `>= _thresh + 1`
    # tight, and both are strictly above the general gate that had already
    # fired. It could append a reason string; it could never change a verdict.
    #
    # Two independent reasons it goes rather than gets re-tuned. It asserted a
    # premise nothing verified -- `abi_impact` is a regex over the section's
    # PROSE, so a section merely CITING a header as evidence trips it, and one
    # such verdict cost a session the reads to disprove an ABI change it never
    # proposed. And the calibration measured this exact feature at r = -0.22
    # against turns (see the note above): sections mentioning ABI ran SHORTER,
    # so the one arm whose sign was measured pointed the wrong way.
    #
    # `abi_impact` is still computed and still REPORTED in `complexity` below.
    # The signal stays auditable and a future recalibration can use it; it just
    # no longer authors a reason for a verdict it did not cause.
    complexity = {
        "files": len(likely_files) + len(input_files),
        "subsystems": subsystems,
        "open_items": len(open_items),
        "abi_impact": abi_impact,
        "verdict": ("SPLIT-RECOMMENDED (" + "; ".join(split_reasons) + ")")
        if split_reasons else "fits-one-context",
        # P3.1: overriding a SPLIT-RECOMMENDED verdict requires a STRUCTURED
        # preflight waiver (see validate_split_waiver), not a free-form
        # "cohesive" -- an over-large section that skips the split is the
        # upstream half of the context-cap.
        "waiver_required": bool(split_reasons),
    }

    # PROVENANCE OF THIS SECTION, surfaced where the decision is actually made.
    # The spawn-chain depth was computed correctly and consulted by nobody: the
    # only readers were two CLI verbs nothing calls automatically, so a run
    # could sit at depth 3 and never learn it. This is deliberately INFORMATION,
    # not a gate -- a refusal here would push a legitimate finding into a park,
    # and parks in closed sections are the 60 stranded items this whole change
    # set exists to stop creating. The run decides; it just decides knowing.
    _chains = spawn_chains(text.split("\n"))
    _mine = _chains.get(n)
    spawn = None
    if _mine:
        spawn = {"parent": _mine[0], "kind": _mine[1], "review_depth": _mine[2],
                 "limit": SPAWN_CHAIN_LIMIT}
        if _mine[1] == "review" and _mine[2] >= SPAWN_CHAIN_LIMIT:
            spawn["note"] = (
                "this section is a deep review-spawn: before creating ANOTHER "
                "from its review, answer user_impact (what a user hits if it is "
                "not done) and not_parkable. Prefer fixing the finding here and "
                "naming it as an `- [x]` item.")

    manifest = {
        "todo": todo_rel, "section": n, "heading": heading,
        "spawn_chain": spawn,
        "open_items": open_items, "done_items": done_items,
        "xrefs": xrefs, "likely_files": likely_files,
        "input_files": input_files, "relevant_tests": tests,
        "required_gates": gates,
        # Working-tree content hashes (see the content_hashes call above). Key
        # kept as blob_hashes for reader compatibility; values are sha256 of
        # current bytes, not index blobs.
        "blob_hashes": blobs,
        "complexity": complexity,
        "enrich_with": ("kernel-explorer" if (kernelish or bootish)
                        else "section-context-mapper"),
    }
    out = json.dumps(manifest, indent=1)
    # --out PATH persists the manifest so evidence-bundle.py --manifest <PATH>
    # can consume it (the chain was broken: the manifest was only ever printed,
    # and piping it through head truncated the JSON so no bundle was produced).
    if "--out" in argv:
        dest = Path(argv[argv.index("--out") + 1])
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(out, encoding="utf-8")
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
