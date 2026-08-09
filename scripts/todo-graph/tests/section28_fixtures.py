"""Section 28 fixtures: section-level readiness verbs over the resolved graph.

Driven by `tests/test_build.sh` (sub-test 28), which turns each PASS/FAIL line
into a t_pass/t_fail. Kept as a module rather than an inline heredoc because
the mutation controls need to re-exec a PATCHED copy of query.py, which is
unreadable squeezed into shell quoting.

The corpus is SYNTHETIC and written to a temp tree rather than derived from
`build/todo-cache.json`, for two reasons: `classify_section` reads stamps off
disk, so the fixture must own real markdown; and the live corpus cannot
exercise `unresolved` or `dangling` at all (it currently has zero of each),
which are precisely the states that must not silently read as satisfied.

EVERY RULE CLAUSE HAS A CONTROL THAT MUST FIRE. A fixture that passes both
with and against the rule proves nothing, and three of these were rewritten
after the first version's dedup control passed unchanged -- the dedup turned
out to be layered, and the clause under test was not the one being reverted.
"""
import os, sys, tempfile

REPO = os.environ.get("REPO_ROOT", os.getcwd())
sys.path.insert(0, os.path.join(REPO, "scripts", "todo-graph"))
import query as q


def md(stamps_by_section):
    """Render a minimal TODO whose sections carry the requested stamps."""
    out = ["# Fixture", ""]
    for n, kinds in sorted(stamps_by_section.items()):
        out.append(f"## {n}. Section {n}")
        out.append("")
        if "V" in kinds:
            out.append("> **Verified:** 2026-08-09 -- fixture")
        if "Q" in kinds:
            out.append("> **Quality reviewed:** 2026-08-09 -- fixture")
        if "D" in kinds:
            out.append("> **Deferred:** 2026-08-09 -- fixture")
        if "DR" in kinds:
            out.append("> **Deferred:** awaiting-hardware -- fixture")
        out.append("")
    return "\n".join(out) + "\n"


def node(path, sections, stamps):
    return {
        "file_path": path,
        "id": path.replace("/", "-").replace(".md", ""),
        "title": os.path.basename(path),
        "domain": path.split("/")[1] if "/" in path else "",
        "status": "active",
        "depends_on": [],
        "sections": sections,
        "_stamps": stamps,
    }


def sec(n, status, deps=None, deliverable="d"):
    return {"n": n, "status": status, "deliverable": deliverable,
            "depends_on": deps or []}


def build(nodes):
    root = tempfile.mkdtemp(prefix="s28-")
    for n in nodes:
        p = os.path.join(root, n["file_path"])
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(md(n.pop("_stamps")))
    return root, q.Ctx(nodes, root, quiet=True)


def rows(ctx, verb):
    fn = q.SUBCOMMANDS[verb][0]
    r, _cols = fn(ctx, None)
    return r


results = []


def check(name, got, want):
    ok = got == want
    results.append((ok, name, got, want))
    return ok


# ---- 28a: an UNSTAMPED [x] target does not satisfy; stamping it does -------
def corpus_a(target_stamps):
    return [
        node("todo/00-a/TODO-01-src.md",
             [sec(1, " ", [{"target": "TODO-02-tgt", "sections": [1]}])],
             {1: set()}),
        node("todo/00-a/TODO-02-tgt.md", [sec(1, "x")], {1: target_stamps}),
    ]


_, ctx = build(corpus_a(set()))
check("28a unstamped [x] target is unmet",
      [r["section"] for r in rows(ctx, "section-ready")], [])
_, ctx = build(corpus_a({"V", "Q"}))
check("28a-mut stamped [x] target satisfies",
      [r["section"] for r in rows(ctx, "section-ready")], [1])

# ---- 28b: a Deferred-parked [/] target satisfies ---------------------------
_, ctx = build([
    node("todo/00-a/TODO-01-src.md",
         [sec(1, " ", [{"target": "TODO-02-tgt", "sections": [1]}])], {1: set()}),
    node("todo/00-a/TODO-02-tgt.md", [sec(1, "/")], {1: {"D"}}),
])
check("28b deferred-parked [/] target satisfies",
      [r["section"] for r in rows(ctx, "section-ready")], [1])

# ---- 28j: a recoverable awaiting-* deferral is BLOCKED, not satisfied ------
_, ctx = build([
    node("todo/00-a/TODO-01-src.md",
         [sec(1, " ", [{"target": "TODO-02-tgt", "sections": [1]}])], {1: set()}),
    node("todo/00-a/TODO-02-tgt.md", [sec(1, "/")], {1: {"DR"}}),
])
check("28j awaiting-* deferral is unmet",
      [r["section"] for r in rows(ctx, "section-ready")], [])
check("28j awaiting-* reported as blocked",
      [r["blocked_by"] for r in rows(ctx, "section-blocked")],
      ["TODO-02-tgt §1(blocked)"])

# ---- 28c: an unmet self prerequisite suppresses section-ready --------------
def corpus_c(self_status, self_stamps):
    return [
        node("todo/00-a/TODO-01-src.md",
             [sec(1, self_status),
              sec(2, " ", [{"target": "TODO-02-tgt", "sections": [1]},
                           {"target": "self", "sections": [1]}])],
             {1: self_stamps, 2: set()}),
        node("todo/00-a/TODO-02-tgt.md", [sec(1, "x")], {1: {"V", "Q"}}),
    ]


_, ctx = build(corpus_c(" ", set()))
check("28c unmet self prerequisite suppresses ready",
      [r["section"] for r in rows(ctx, "section-ready")], [])
_, ctx = build(corpus_c("x", {"V", "Q"}))
check("28c-mut satisfied self prerequisite lets it through",
      [r["section"] for r in rows(ctx, "section-ready")], [2])

# ---- 28d: an ambiguous bare stem is refused, not bound ---------------------
_, ctx = build([
    node("todo/00-a/TODO-01-src.md",
         [sec(1, " ", [{"target": "TODO-09", "sections": [1]}])], {1: set()}),
    node("todo/01-b/TODO-09-dup.md", [sec(1, "x")], {1: {"V", "Q"}}),
    node("todo/02-c/TODO-09-dup.md", [sec(1, "x")], {1: {"V", "Q"}}),
])
check("28d ambiguous bare stem refused",
      [r["blocked_by"] for r in rows(ctx, "section-blocked")],
      ["TODO-09 §1(unresolved)"])

# ---- 28e: a target section number absent from the target file --------------
_, ctx = build([
    node("todo/00-a/TODO-01-src.md",
         [sec(1, " ", [{"target": "TODO-02-tgt", "sections": [99]}])], {1: set()}),
    node("todo/00-a/TODO-02-tgt.md", [sec(1, "x")], {1: {"V", "Q"}}),
])
check("28e dangling target section",
      [r["blocked_by"] for r in rows(ctx, "section-blocked")],
      ["TODO-02-tgt §99(dangling)"])

# ---- 28i: an empty section list uses the whole-file verdict ----------------
def corpus_i(tgt_status, tgt_stamps):
    return [
        node("todo/00-a/TODO-01-src.md",
             [sec(1, " ", [{"target": "TODO-02-tgt", "sections": []}])], {1: set()}),
        node("todo/00-a/TODO-02-tgt.md",
             [sec(1, tgt_status), sec(2, tgt_status)],
             {1: tgt_stamps, 2: tgt_stamps}),
    ]


_, ctx = build(corpus_i(" ", set()))
check("28i whole-file target open",
      [r["blocked_by"] for r in rows(ctx, "section-blocked")],
      ["TODO-02-tgt(file-open)"])
_, ctx = build(corpus_i("x", {"V", "Q"}))
check("28i-mut whole-file target done satisfies",
      [r["section"] for r in rows(ctx, "section-ready")], [1])

# ---- 28g: a repeated target section counts ONCE ----------------------------
_, ctx = build([
    node("todo/00-a/TODO-01-src.md",
         [sec(1, " ", [{"target": "TODO-02-tgt", "sections": [1, 1]}])], {1: set()}),
    node("todo/00-a/TODO-02-tgt.md", [sec(1, " ")], {1: set()}),
])
check("28g repeated target section counts once",
      [r["inbound_count"] for r in rows(ctx, "section-blocking")], [1])

# ---- 28h: the file-level trio are UNCHANGED on a section-only corpus -------
_, ctx = build([
    node("todo/00-a/TODO-01-src.md",
         [sec(1, " ", [{"target": "TODO-02-tgt", "sections": [1]}])], {1: set()}),
    node("todo/00-a/TODO-02-tgt.md", [sec(1, "x")], {1: {"V", "Q"}}),
])
check("28h file-level blocked unchanged (empty)", rows(ctx, "blocked"), [])
check("28h file-level blocking unchanged (empty)", rows(ctx, "blocking"), [])

# ---- 28f: a token resolving to a domain INDEX.md is not a node -------------
_, ctx = build([
    node("todo/00-a/TODO-01-src.md",
         [sec(1, " ", [{"target": "01-b", "sections": []}])], {1: set()}),
    node("todo/01-b/TODO-02-tgt.md", [sec(1, "x")], {1: {"V", "Q"}}),
])
_r = rows(ctx, "section-blocked")
check("28f domain-index target is non-node, domain kept in label",
      [r["blocked_by"] for r in _r], ["01-b/INDEX(non-node)"])

# ---- 28k: a BLOCKED source is never `ready`, even with satisfied deps ------
# Found by the section-28 Codex adversarial pass: `_open_sources` returns
# every non-DONE source, and BLOCKED (a recoverable `awaiting-*` deferral) is
# not DONE -- so the verb pointed at work explicitly parked on something
# external. The live corpus has no such source, which is exactly why it needs
# a fixture rather than a measurement.
def corpus_k(src_stamps):
    return [
        node("todo/00-a/TODO-01-src.md",
             [sec(1, "/", [{"target": "TODO-02-tgt", "sections": [1]}])],
             {1: src_stamps}),
        node("todo/00-a/TODO-02-tgt.md", [sec(1, "x")], {1: {"V", "Q"}}),
    ]


_, ctx = build(corpus_k({"DR"}))
check("28k awaiting-* SOURCE is not ready despite satisfied deps",
      [r["section"] for r in rows(ctx, "section-ready")], [])
_, ctx = build(corpus_k(set()))
check("28k-mut the same source unparked IS ready",
      [r["section"] for r in rows(ctx, "section-ready")], [1])

# ---- 28l: a target aliasing the SOURCE file is treated as self -------------
# 8 live groups name their own file by code (`TODO-04`) rather than `self`.
# Counted as cross-file they satisfy section-ready's eligibility test with a
# same-file edge, and inflate section-blocking with pressure no other file
# applied.
def corpus_l(alias_done):
    """Section 2 depends on section 1 of its OWN file, written as a code.

    `alias_done` decides whether that prerequisite is satisfied, and the two
    settings isolate different halves of the defect: unsatisfied shows the
    edge is reported as a SELF dependency, satisfied shows it must not confer
    cross-file ELIGIBILITY on `section-ready`.
    """
    return [
        node("todo/00-a/TODO-01-src.md",
             [sec(1, "x" if alias_done else " "),
              sec(2, " ", [{"target": "TODO-01-src", "sections": [1]}])],
             {1: ({"V", "Q"} if alias_done else set()), 2: set()}),
    ]


_, ctx = build(corpus_l(False))
check("28l self-alias is reported as a self dependency",
      [r["blocked_by"] for r in rows(ctx, "section-blocked")], ["self §1(open)"])
check("28l self-alias applies no cross-file blocking pressure",
      rows(ctx, "section-blocking"), [])
_, ctx = build(corpus_l(True))
check("28l a SATISFIED self-alias confers no cross-file eligibility",
      [r["section"] for r in rows(ctx, "section-ready")], [])

# ---- 28m: rows with a null section number cannot collide -------------------
# The cache contract permits a null `sections[].n` and does not require nulls
# to be unique, so keying on (file_path, n) alone would merge two such rows'
# edges and understate a dependent count.
def corpus_m():
    return [
        node("todo/00-a/TODO-01-src.md",
             [{"n": None, "status": " ", "deliverable": "x",
               "depends_on": [{"target": "TODO-02-tgt", "sections": [1]}]},
              {"n": None, "status": " ", "deliverable": "y",
               "depends_on": [{"target": "TODO-02-tgt", "sections": [1]}]},
              sec(1, " ", [{"target": "TODO-02-tgt", "sections": [1]}])],
             {1: set()}),
        node("todo/00-a/TODO-02-tgt.md", [sec(1, " ")], {1: set()}),
    ]


_, ctx = build(corpus_m())
check("28m null-numbered rows stay DISTINCT and all three count",
      [(r["section"], r["inbound_count"]) for r in rows(ctx, "section-blocking")],
      [(1, 3)])
check("28m null-numbered sources are reported with a null section",
      sorted((r["section"] is None) for r in rows(ctx, "section-blocked")),
      [False, True, True])

# ---- 28n: an EMPTY-section self alias means the whole SOURCE file ----------
# 3 of the 8 live self-aliases carry an empty section list. Iterating the
# section numbers alone dropped the edge entirely, so a section could hold an
# open whole-file prerequisite on its own file and still be called ready.
def corpus_n(other_done):
    return [
        node("todo/00-a/TODO-01-src.md",
             [sec(1, "x" if other_done else " "),
              sec(2, " ", [{"target": "TODO-02-tgt", "sections": [1]},
                           {"target": "TODO-01-src", "sections": []}])],
             {1: ({"V", "Q"} if other_done else set()), 2: set()}),
        node("todo/00-a/TODO-02-tgt.md", [sec(1, "x")], {1: {"V", "Q"}}),
    ]


_, ctx = build(corpus_n(False))
check("28n empty self-alias keeps an open whole-file prerequisite visible",
      [r["blocked_by"] for r in rows(ctx, "section-blocked")],
      ["self(file-open)"])
check("28n empty self-alias suppresses ready while the file is open",
      [r["section"] for r in rows(ctx, "section-ready")], [])

# ---- MUTATION CONTROLS: revert a rule clause, the fixture must FAIL --------
import importlib.util


def mutated(needle, replacement):
    src = open(os.path.join(REPO, "scripts", "todo-graph", "query.py"),
               encoding="utf-8").read()
    if needle not in src:
        raise SystemExit(f"MUTATION NEEDLE MISSING: {needle!r}")
    spec = importlib.util.spec_from_loader("q_mut", loader=None)
    mod = importlib.util.module_from_spec(spec)
    mod.__file__ = os.path.join(REPO, "scripts", "todo-graph", "query.py")
    sys.modules["q_mut"] = mod
    exec(compile(src.replace(needle, replacement), mod.__file__, "exec"),
         mod.__dict__)
    return mod


def mrows(mod, nodes, verb):
    root = tempfile.mkdtemp(prefix="s28m-")
    for n in nodes:
        p = os.path.join(root, n["file_path"])
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(md(n.pop("_stamps")))
    c = mod.Ctx(nodes, root, quiet=True)
    return mod.SUBCOMMANDS[verb][0](c, None)[0]


# F1 reverted: satisfaction from the raw marker instead of the classifier.
m = mutated('cls = deps.section_class[tkey]\n    if cls == deps.DONE:',
            'cls = deps.section_class[tkey]\n    if True:')
check("CONTROL F1 reverting to the raw marker makes 28a pass wrongly",
      [r["section"] for r in mrows(m, corpus_a(set()), "section-ready")], [1])

# F2 reverted: self prerequisites ignored.
m = mutated('        if any(not d["satisfied"] for d in deps.selfdeps.get(key, [])):\n'
            '            continue\n',
            '')
check("CONTROL F2 ignoring self deps makes 28c pass wrongly",
      [r["section"] for r in mrows(m, corpus_c(" ", set()), "section-ready")], [2])

# ---- 28g2: the same target reached through TWO groups counts once ----------
def corpus_g2():
    return [
        node("todo/00-a/TODO-01-src.md",
             [sec(1, " ", [{"target": "TODO-02-tgt", "sections": [1]},
                           {"target": "TODO-02-tgt", "sections": [1]}])], {1: set()}),
        node("todo/00-a/TODO-02-tgt.md", [sec(1, " ")], {1: set()}),
    ]


_, ctx = build(corpus_g2())
check("28g2 same target via two groups counts once",
      [r["inbound_count"] for r in rows(ctx, "section-blocking")], [1])
check("28g2 blocked_by names the target once",
      [r["blocked_by"] for r in rows(ctx, "section-blocked")],
      ["TODO-02-tgt §1(open)"])

# ---- 28g3: a repeated SELF section is listed once --------------------------
def corpus_g3():
    return [
        node("todo/00-a/TODO-01-src.md",
             [sec(1, " "),
              sec(2, " ", [{"target": "TODO-02-tgt", "sections": [1]},
                           {"target": "self", "sections": [1, 1]}])],
             {1: set(), 2: set()}),
        node("todo/00-a/TODO-02-tgt.md", [sec(1, "x")], {1: {"V", "Q"}}),
    ]


_, ctx = build(corpus_g3())
check("28g3 repeated self section listed once",
      [r["blocked_by"] for r in rows(ctx, "section-blocked")],
      ["self §1(open)"])

# F4/L1 reverted: no in-group dedup -- the SELF branch has no other dedup.
m = mutated('numbers = list(dict.fromkeys(_safe_list(grp.get("sections"))))',
            'numbers = list(_safe_list(grp.get("sections")))')
check("CONTROL F4-L1 dropping in-group dedup duplicates the self prerequisite",
      [r["blocked_by"] for r in mrows(m, corpus_g3(), "section-blocked")],
      ["self §1(open),self §1(open)"])

# F4/L2 reverted: no per-source dedup -- one dependent scores twice.
_l2 = ('    for key, edges in out.cross.items():\n'
       '        seen = set()\n'
       '        kept = []\n'
       '        for e in edges:\n'
       '            eid = (e["target_path"] or e["token"], e["target_section"])\n'
       '            if eid in seen:\n'
       '                continue\n'
       '            seen.add(eid)\n'
       '            kept.append(e)\n'
       '        out.cross[key] = kept\n')
m = mutated(_l2, '')
check("CONTROL F4-L2 dropping per-source dedup double-counts one dependent",
      [r["inbound_count"] for r in mrows(m, corpus_g2(), "section-blocking")], [2])

# A2 reverted: any non-DONE source treated as runnable.
m = mutated('        if deps.section_class.get(key) != "NEEDS_WORK":\n            continue\n', '')
check("CONTROL A2 accepting any non-DONE source reports a BLOCKED one ready",
      [r["section"] for r in mrows(m, corpus_k({"DR"}), "section-ready")], [1])

# A3 reverted: only the literal `self` token routed to the self branch.
m = mutated('if token == "self" or resolved == src:', 'if token == "self":')
check("CONTROL A3 ignoring self-aliases turns a same-file edge into cross-file",
      [r["section"] for r in mrows(m, corpus_l(True), "section-ready")], [2])

# A4 reverted: null rows keyed by n alone, so the two collide into one.
m = mutated('    return (path, n if n is not None else f"~{ordinal}")',
            '    return (path, n)')
check("CONTROL A4 keying null rows by n alone merges two sources into one",
      [(r["section"], r["inbound_count"]) for r in mrows(m, corpus_m(),
                                                         "section-blocking")],
      [(1, 2)])

# A5 reverted: the empty self-alias iterates section numbers only, so a
# whole-file self prerequisite vanishes and the section is called ready.
m = mutated('                    for m in (numbers or [None]):\n'
            '                        if m is None:\n'
            '                            cls = out.file_class.get(src)',
            '                    for m in (numbers):\n'
            '                        if m is None:\n'
            '                            cls = out.file_class.get(src)')
check("CONTROL A5 dropping the empty self-alias reports the section ready",
      [r["section"] for r in mrows(m, corpus_n(False), "section-ready")], [2])

for ok, name, got, want in results:
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else f" | got={got!r} want={want!r}"))
print("SUMMARY", sum(1 for r in results if r[0]), "/", len(results))
