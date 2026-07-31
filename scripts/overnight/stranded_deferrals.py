#!/usr/bin/env python3
"""P6.2 -- stranded-deferral audit (READ-ONLY diagnostic).

Surfaces terminal-park `[/]` checklist items whose cross-TODO XREF owner section
has now SHIPPED (both `> **Verified:**` and `> **Quality reviewed:**` stamps),
yet the item is still parked -- the "cross-TODO work that became runnable but was
never re-opened" gap. It ONLY PRINTS a human-reviewable list; it never edits a
TODO and never re-opens anything itself. STALE-HEADER FIX 2026-07-31: the
acting pieces are no longer all deferred -- P6.1 (owner-side sweep) is
MANDATORY at implement-todo-section step 18, and P6.3 (fixpoint gate, `--gate`
mode below) GATES run completion since 2026-07-27. Only P6.4 (bulk backfill)
remains deliberately unbuilt. The original caution stands: the naive
section-stamp signal the plan first proposed over-matched ~6x on the live tree
(171 hits, 143 of them self-XREFs to already-open `[ ]` items), so nothing acts
on this until the audit is proven accurate on real data.

Precision rules (deliberately conservative -- a read-only audit should
under-report an ambiguous case rather than nominate phantom work):
  * ITEM level: only `- [/]` checklist items (terminal-park), not section stamps.
  * CROSS-TODO only: an XREF to the item's OWN file is skipped (a self-deferral
    to a later same-file section is not the stranding gap).
  * awaiting-<token> excluded: those are recoverable-park (BLOCKED), already
    re-checked by the triage oracle -- not terminal-park.
  * OWNER SHIPPED = target section carries BOTH Verified AND Quality-reviewed (a
    terminal-Deferred owner punted too, so it does NOT unblock the dependent).
  * Only emits when a (target_file, target_section) pair resolves unambiguously.

Suggested action per item (ADVISORY only -- the human decides):
  * clean  -- item text says the work is OWNED/DONE by the owner ("deferred to",
     "owned by", "shipped by", "already shipped"): confirm-done; the owner did
     it, the dependent never does. Do NOT re-open (would duplicate work).
  * reopen -- item reads as in-scope work that was BLOCKED on the owner
     ("blocked on", "pending", "was blocked", "unblocked once"): flip to open.
  * review -- neither pattern matched; a human classifies.

Usage:
  stranded_deferrals.py            human-readable table + summary
  stranded_deferrals.py --json     machine-readable list
  stranded_deferrals.py --selftest
"""
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / ".claude/hooks"))
import sequencer_triage as st  # noqa: E402  (authoritative section-stamp oracle)

_SIGN = "§"  # the section-sign glyph, built here so source carries no bare glyph+digit

# A TODO path reference inside item prose: an optional `../`, a domain dir, and a
# TODO-NN basename, with or without `.md` / a markdown-link wrapper.
_TODO_REF_RE = re.compile(
    r"(?:\[[^\]]*\]\()?((?:\.\./)?[\w./-]*?TODO-[\w-]+?(?:\.md)?)(?=[\s)`\]#" + _SIGN + r"]|$)")
_SECTION_RE = re.compile(_SIGN + r"\s*(\d+)")
_AWAITING_RE = re.compile(r"\bawaiting-[a-z0-9-]+\b", re.IGNORECASE)

_CLEAN_HINT = re.compile(
    r"\b(?:deferred to|owned by|shipped by|already shipped|wired in|owner did)\b",
    re.IGNORECASE)
# "blocked on <owner>" language: the item was gated on the XREF owner, which has
# now shipped -> a flip candidate (if nothing ELSE blocks it).
_BLOCKED_ON_RE = re.compile(r"\bblocked on\b|\bwas blocked\b|\bunblocked\b", re.IGNORECASE)
# A LIVE dependency the shipped owner does NOT resolve -- so the item stays
# blocked even though its XREF owner shipped. Owner-shipped is necessary but not
# SUFFICIENT (verified on the real tree: TODO-14 registry hive-load items XREF a
# shipped early-entropy section yet remain blocked on "needs C:", boot-mount, GPF
# history). Any of these present -> classify `blocked`, never auto-flip.
_OTHER_BLOCKER_RE = re.compile(
    r"(?:\b(?:unimplemented|not implemented|coupled|needs\b|pending\b|awaiting|"
    r"no rollback|not power-fail|no enforcement|false security|GPF|boot-mount|"
    r"needs\s+C:|not yet|blocker|not_supported|still open)\b"
    r"|until\b.*\bexists?\b"          # "return ERROR_NOT_SUPPORTED until the hive I/O bodies exist"
    r"|DEFERRED\s*\("                 # "DEFERRED (dual-log)", "DEFERRED (with load trio)" -- coupled unit
    r"|ships? with\b"                 # "per-key provenance ships with the set path" (set path deferred)
    r")", re.IGNORECASE)


def _impl_todos(root: Path):
    for p in sorted((root / "todo").rglob("TODO-*.md")):
        rel = str(p.relative_to(root))
        if st.is_impl_todo(rel):
            yield p


def _resolve_target(ref: str, root: Path, src: Path):
    """Resolve a prose TODO ref (e.g. '04-drivers-hardware/TODO-10-usb-stack.md',
    '01-boot-platform/TODO-07', or a '../'-relative link) to an existing file,
    best-effort. Returns a Path or None when it does not resolve uniquely."""
    ref = ref.strip().strip("`")
    cands = []
    bases = [src.parent, root / "todo", root]
    stems = [ref]
    if not ref.endswith(".md"):
        stems.append(ref)  # bare TODO-NN -> glob the full basename below
    for base in bases:
        for stem in stems:
            try:
                if stem.endswith(".md"):
                    q = base / stem
                    if q.exists():
                        cands.append(q)
                else:
                    cands += list(base.glob(stem + "*.md"))
            except Exception:
                continue
    uniq = sorted({c.resolve() for c in cands if c.exists() and c.suffix == ".md"})
    return uniq[0] if len(uniq) == 1 else None


def _classify(text: str) -> tuple:
    """(action, blocker_evidence). Conservative ladder -- only `flip` is a
    safe-to-re-open verdict:
      clean   -- the owner did the work; do NOT re-open (would duplicate).
      blocked -- a live dependency beyond the shipped owner remains; leave parked.
      flip    -- gated on the (now-shipped) owner, nothing else blocks it -> [ ].
      review  -- none matched (often a correctly-partial [/]); human decides.
    """
    # Blocker FIRST: a live dependency overrides any incidental clean/flip prose
    # (e.g. "wired in X ... PENDING deep-copy setters" is partial, not done;
    # "DEFERRED (... needs boot-mount ...) tracked in header" is blocked, not clean).
    ob = _OTHER_BLOCKER_RE.search(text)
    if ob:
        return ("blocked", ob.group(0))
    if _CLEAN_HINT.search(text):
        return ("clean", "")
    if _BLOCKED_ON_RE.search(text):
        return ("flip", "")
    return ("review", "")


def _stamp_cache():
    cache = {}

    def stamps(path: Path):
        k = str(path)
        if k not in cache:
            cache[k] = st.section_stamps(str(path))
        return cache[k]
    return stamps


def _containing_section(lines, idx):
    """Section number of the `## N.` heading at or above line index `idx`."""
    for j in range(idx, -1, -1):
        m = st.SECTION_HEADING_RE.match(lines[j] + "\n")
        if m:
            return int(m.group(1))
    return None


def _section_is_parked(stamps: set) -> bool:
    """True when a section is DONE-equivalent to the triage oracle (so the
    fixpoint loop never re-visits it): shipped+reviewed (V AND Q), or a TERMINAL
    Deferred stamp (D without an awaiting-* recoverable marker DR). An item in
    such a section will NOT flip naturally on an overnight run -- it is genuinely
    stranded."""
    return ({"V", "Q"} <= stamps) or ("D" in stamps and "DR" not in stamps)


def audit(root: Path) -> list:
    """Return the stranded-candidate list (see module docstring)."""
    stamps_of = _stamp_cache()
    out = []
    for src in _impl_todos(root):
        try:
            lines = src.read_text(encoding="utf-8").splitlines()
        except OSError:
            continue
        src_stamps = stamps_of(src)
        for i, line in enumerate(lines, 1):
            if not line.startswith("- [/]"):
                continue
            if _AWAITING_RE.search(line):
                continue  # recoverable-park, not terminal-park
            secs = [int(m.group(1)) for m in _SECTION_RE.finditer(line)]
            if not secs:
                continue
            for m in _TODO_REF_RE.finditer(line):
                tgt = _resolve_target(m.group(1), root, src)
                if not tgt or tgt.resolve() == src.resolve():
                    continue  # unresolved or self-XREF
                after = line[m.end():]
                sm = _SECTION_RE.search(after)
                sec = int(sm.group(1)) if sm else (secs[0] if len(secs) == 1 else None)
                if sec is None:
                    continue
                if {"V", "Q"} <= stamps_of(tgt).get(sec, set()):
                    csec = _containing_section(lines, i - 1)
                    parked = _section_is_parked(src_stamps.get(csec, set())) \
                        if csec is not None else False
                    action, blocker = _classify(line)
                    out.append({
                        "source": str(src.relative_to(root)),
                        "line": i,
                        "target": str(tgt.relative_to(root)),
                        "section": sec,
                        "action": action,
                        "blocker": blocker,
                        # stranded=True: the item's OWN section is DONE-parked, so
                        # the fixpoint loop never re-visits it -> it will NOT flip
                        # naturally on an overnight run. stranded=False: its section
                        # is still worked, so it MAY flip (model-dependent).
                        "stranded": parked,
                        "item": line[6:120].strip(),
                    })
                break  # one target per item is enough to nominate it
    seen, uniq = set(), []
    for e in out:
        k = (e["source"], e["line"], e["target"], e["section"])
        if k not in seen:
            seen.add(k)
            uniq.append(e)
    return uniq


def _selftest() -> int:
    import tempfile
    fails = []
    s3 = _SIGN + "3"
    s2 = _SIGN + "2"
    s1 = _SIGN + "1"
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        (root / "todo/02-kernel-core").mkdir(parents=True)
        (root / "todo/01-boot-platform").mkdir(parents=True)
        # Owner section 3 in TODO-05: SHIPPED (Verified + Quality-reviewed).
        (root / "todo/02-kernel-core/TODO-05-x.md").write_text(
            "---\nid: TODO-05\nstatus: active\ndomain: x\ntitle: t\n---\n"
            "## 3. Owner section\n- [x] do it\n"
            "> **Verified:** 2026-01-01 | done\n"
            "> **Quality reviewed:** 2026-01-01 | ok\n")
        # Owner section 2 in TODO-06: NOT shipped (no stamps).
        (root / "todo/02-kernel-core/TODO-06-y.md").write_text(
            "---\nid: TODO-06\nstatus: active\ndomain: x\ntitle: t\n---\n"
            "## 2. Unshipped\n- [ ] later\n")
        (root / "todo/01-boot-platform/TODO-09-z.md").write_text(
            "---\nid: TODO-09\nstatus: active\ndomain: b\ntitle: t\n---\n"
            "## 1. S\n"
            f"- [/] blocked on 02-kernel-core/TODO-05 {s3} for the token wiring\n"  # shipped, no other blocker -> flip
            f"- [/] deferred to 02-kernel-core/TODO-05-x.md {s3} owned there\n"     # -> clean
            f"- [/] blocked on 02-kernel-core/TODO-05 {s3} but needs C: mounted\n"  # shipped owner BUT other blocker -> blocked
            f"- [/] blocked on 02-kernel-core/TODO-06 {s2} (unshipped)\n"           # unshipped owner -> excluded entirely
            f"- [/] self thing -> XREF: {s1} awaiting-hardware\n")               # self+awaiting -> excluded
        res = audit(root)
        got = {(e["section"], e["action"]) for e in res if e["section"] == 3}
        actions = sorted(e["action"] for e in res)
        if actions != ["blocked", "clean", "flip"]:
            fails.append(f"expected [blocked, clean, flip], got {actions}: {res}")
        if (3, "flip") not in got:
            fails.append(f"missing shipped-owner flip candidate: {got}")
        if (3, "blocked") not in got:
            fails.append(f"other-blocker item must be 'blocked' not 'flip': {got}")
        if any(e["section"] == 2 for e in res):
            fails.append("unshipped owner (section 2) must NOT be stranded")
    if fails:
        for f in fails:
            print("FAIL:", f, file=sys.stderr)
        return 1
    print("stranded_deferrals selftest OK (item-level, cross-TODO, owner-shipped)")
    return 0


# ---------------------------------------------------------------------------
# P6.1 / P6.3 -- dispositions and the fixpoint gate
# ---------------------------------------------------------------------------
#
# The audit above only REPORTS. Two consumers act on it:
#
#   --owner <todo> [--section N]   P6.1 owner-side sweep. When a section ships,
#                                  list the inbound stranded items that section
#                                  just unblocked so the shipping pass re-opens
#                                  them AT THE MOMENT they become runnable.
#   --gate                         P6.3 fixpoint gate. Exit 1 while any STRANDED
#                                  item lacks a current disposition, so an
#                                  unattended run cannot declare completion with
#                                  unblocked-but-parked work outstanding.
#
# Why a gate cannot wedge the run: `park` is always an available disposition
# (with a reason), so there is a legal way forward for every item. The gate
# forces a DECISION, never a particular decision.
#
# Precision evidence (2026-07-27): a 10-item hand sample across all three action
# labels found 0 false positives -- 6 verified genuinely-unblocked at file:line
# (RtlCaptureStackBackTrace, media_role_locate_blackbox_fs,
# SYSTEM_KERNEL_CONFIG_INFORMATION, csprng_fill/csprng_crypto_ok,
# SeSinglePrivilegeCheck, eif_decompress_segment) and 2 ambiguous-but-correctly
# -flagged. That retired the ~6x over-match which kept P6.3 deferred. The sample
# also showed the `action` label is over-CONSERVATIVE: items tagged `blocked`
# were in fact unblocked with stale item text, so `flip` badly understates the
# actionable set -- which is why the gate keys on `stranded`, not on `action`.

DISPOSITIONS_REL = ".claude/state/stranded-dispositions.json"
VALID_ACTIONS = ("reopen", "done", "park")


def item_key(entry: dict) -> str:
    """Content-bound identity for a stranded item.

    Deliberately EXCLUDES the line number -- an item that merely moves down the
    file keeps its disposition -- but INCLUDES the item text, so editing what
    the item says re-opens the question rather than silently inheriting a stale
    verdict.
    """
    import hashlib
    raw = "\x1f".join((
        entry.get("source", ""), entry.get("target", ""),
        str(entry.get("section", "")),
        " ".join((entry.get("item") or "").split()),
    ))
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()[:16]


def load_dispositions(root: Path) -> dict:
    try:
        d = json.loads((root / DISPOSITIONS_REL).read_text(encoding="utf-8"))
        return d if isinstance(d, dict) else {}
    except Exception:
        return {}


def save_disposition(root: Path, key: str, action: str, reason: str) -> None:
    p = root / DISPOSITIONS_REL
    d = load_dispositions(root)
    d[key] = {"action": action, "reason": reason}
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix(p.suffix + ".tmp")
    tmp.write_text(json.dumps(d, indent=1, sort_keys=True), encoding="utf-8")
    tmp.replace(p)


def cmd_gate(root: Path, res: list) -> int:
    """Exit 0 when every STRANDED item is dispositioned; 1 otherwise."""
    disp = load_dispositions(root)
    pending = [e for e in res if e.get("stranded")
               and disp.get(item_key(e), {}).get("action") not in VALID_ACTIONS]
    if not pending:
        n = sum(1 for e in res if e.get("stranded"))
        print(f"stranded-deferral gate: PASS ({n} stranded item(s), all "
              f"dispositioned).")
        return 0
    print(f"stranded-deferral gate: REFUSED -- {len(pending)} unblocked-but-"
          f"parked item(s) have no disposition. Their XREF owner has SHIPPED "
          f"(both Verified and Quality-reviewed), so fixpoint would never "
          f"revisit them and the OS would be declared complete with this work "
          f"outstanding.", file=sys.stderr)
    print("Disposition each with:\n"
          "  python3 scripts/overnight/stranded_deferrals.py --dispose <key> "
          "--action <reopen|done|park> --reason '<why>'\n"
          "  reopen = the work is now runnable -- FILE IT as a concrete `- [ ]` "
          "item in a section the oracle can still see (an open section, or a "
          "new one via scope-gap Branch B/C/D) with a reciprocal XREF.\n"
          "           Do NOT just flip `- [/]` to `- [ ]` in place: the oracle "
          "classifies on the Implementation Order ROW + section stamps, never "
          "on items, so an in-place flip inside a shipped section is invisible "
          "to the runner AND drops out of this audit (it matches `- [/]` "
          "only) -- invisible to both nets.\n"
          "  done   = the owner already did this work; confirm and close\n"
          "  park   = still genuinely blocked on something else; name it\n",
          file=sys.stderr)
    for e in pending:
        print(f"  [{item_key(e)}] {e['source']}:{e['line']} -> {e['target']} "
              f"sec {e['section']}\n      {e['item'][:150]}", file=sys.stderr)
    return 1


def cmd_owner(res: list, owner: str, section: str | None) -> int:
    """P6.1: inbound stranded items whose owner is this just-shipped section."""
    hits = [e for e in res
            if (owner in e.get("target", ""))
            and (section is None or str(e.get("section")) == str(section))]
    if not hits:
        print(f"owner sweep: no inbound parked items point at {owner}"
              + (f" sec {section}" if section else "") + ".")
        return 0
    print(f"owner sweep: {len(hits)} inbound parked item(s) name {owner}"
          + (f" sec {section}" if section else "")
          + " as their owner. This section shipping is what unblocks them -- "
            "re-open the ones its work actually freed, NOW, while the context "
            "is fresh:")
    for e in hits:
        print(f"  [{item_key(e)}] {e['source']}:{e['line']}  ({e['action']})"
              f"\n      {e['item'][:200]}")
    return 0


def main(argv) -> int:
    if "--selftest" in argv:
        return _selftest()

    if "--dispose" in argv:
        def _opt(name, default=None):
            return argv[argv.index(name) + 1] if name in argv else default
        key = _opt("--dispose")
        action = _opt("--action")
        reason = _opt("--reason", "")
        if action not in VALID_ACTIONS:
            print(f"--action must be one of {VALID_ACTIONS}", file=sys.stderr)
            return 2
        if not reason or len(reason) < 8:
            print("--reason is required (>= 8 chars): a disposition without a "
                  "recorded why is how stranded work goes missing again.",
                  file=sys.stderr)
            return 2
        save_disposition(REPO, key, action, reason)
        print(f"recorded: {key} -> {action} ({reason})")
        return 0

    res = audit(REPO)
    if "--json" in argv:
        print(json.dumps(res, indent=2))
        return 0
    if "--gate" in argv:
        return cmd_gate(REPO, res)
    if "--owner" in argv:
        owner = argv[argv.index("--owner") + 1]
        section = (argv[argv.index("--section") + 1]
                   if "--section" in argv else None)
        return cmd_owner(res, owner, section)
    if not res:
        print("stranded-deferral audit: 0 candidates (no cross-TODO [/] item "
              "whose shipped owner leaves it parked).")
        return 0
    by = {a: sum(1 for e in res if e["action"] == a)
          for a in ("flip", "clean", "blocked", "review")}
    stranded = sum(1 for e in res if e["stranded"])
    print(f"stranded-deferral audit: {len(res)} candidate(s) "
          f"[flip={by['flip']} clean={by['clean']} blocked={by['blocked']} "
          f"review={by['review']}] -- ADVISORY, read-only. No item was changed.")
    print(f"  {stranded} sit in DONE-parked sections (fixpoint never re-visits "
          f"them -> will NOT flip naturally); {len(res) - stranded} in still-worked "
          f"sections. Only `flip` is a safe auto-reopen; `blocked` has a live "
          f"dependency beyond the shipped owner; `clean` is owner-completed.")
    for e in res:
        tag = "STRANDED" if e["stranded"] else "revisit "
        bl = f"  (blocker: {e['blocker']})" if e.get("blocker") else ""
        print(f"  [{e['action']:7}|{tag}] {e['source']}:{e['line']} -> "
              f"{e['target']} sec {e['section']}{bl}\n            {e['item']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
