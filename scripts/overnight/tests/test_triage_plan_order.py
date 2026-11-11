#!/usr/bin/env python3
"""The triage oracle follows the implementation plan (developer tooling stack,
section 18).

`--next` keeps the live cursor file while it has a section the run can start,
then opens the owner of the first plan row that can be built, and falls back
to traversal order when the plan cannot answer. `runnable_sections` decides
which open sections the run may start: a shipped section owes only its review
and is always runnable; one not yet shipped must be on the plan's buildable
list. The plan answer is injected (`ready=`) so the order logic is tested
without building a graph; `plan_ready` itself is tested for its refusal
direction, where a missing tool means "no answer", never "nothing is ready".
"""
import json
import os
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / ".claude/hooks"))
import sequencer_triage as st  # noqa: E402

A = "todo/00-alpha/TODO-01-alpha.md"
B = "todo/01-beta/TODO-01-beta.md"
C = "todo/01-beta/TODO-02-gamma.md"


def write(root, rel, text):
    p = pathlib.Path(root) / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)


def entry(path, domain, statuses):
    return {"file_path": path, "domain": domain,
            "sections": [{"n": n, "status": s, "deliverable": f"s{n}", "depends_on": []}
                         for n, s in statuses]}


def body(ns):
    return "# T\n\n" + "".join(f"## {n}. S{n}\n\n- [ ] item\n\n" for n in ns)


def corpus(root):
    write(root, A, body([1, 2]))
    write(root, B, body([1, 2]))
    write(root, C, body([1]))
    return [entry(A, "00-alpha", [(1, " "), (2, " ")]),
            entry(B, "01-beta", [(1, " "), (2, "x")]),
            entry(C, "01-beta", [(1, " ")])]


def set_cursor(root, path, active=True):
    write(root, st.RUN_STATE, json.dumps({"active": active, "file": path}))


def test_the_first_buildable_plan_row_picks_the_file():
    with tempfile.TemporaryDirectory() as root:
        cache = corpus(root)
        # Traversal order would open A; the plan says only gamma 1 can be built.
        e, cls = st.next_file(cache, root, ready=[(C, 1)])
        assert cls == st.NEEDS_WORK and e["file_path"] == C, e


def test_the_cursor_file_is_kept_while_it_has_a_runnable_section():
    with tempfile.TemporaryDirectory() as root:
        cache = corpus(root)
        set_cursor(root, B)
        # B 2 shipped and owes its review: runnable even though no B row is ready.
        e, _ = st.next_file(cache, root, ready=[(C, 1)])
        assert e["file_path"] == B, e
        # A stopped run's cursor does not hold the run.
        set_cursor(root, B, active=False)
        e, _ = st.next_file(cache, root, ready=[(C, 1)])
        assert e["file_path"] == C, e


def test_the_cursor_file_is_left_when_nothing_in_it_can_start():
    with tempfile.TemporaryDirectory() as root:
        cache = corpus(root)
        set_cursor(root, A)
        e, _ = st.next_file(cache, root, ready=[(C, 1)])
        assert e["file_path"] == C, e


def test_no_plan_answer_falls_back_to_traversal_order():
    with tempfile.TemporaryDirectory() as root:
        cache = corpus(root)
        set_cursor(root, C)
        e, _ = st.next_file(cache, root, ready=None)
        assert e["file_path"] == A, e
        # An empty plan answer (nothing buildable) still never stops the run:
        # the shipped B 2 is the runnable work.
        e, _ = st.next_file(cache, root, ready=[])
        assert e["file_path"] == B, e


def test_a_runnable_review_beats_an_earlier_waiting_file():
    with tempfile.TemporaryDirectory() as root:
        cache = corpus(root)
        # Nothing buildable, no cursor: A waits, B 2 owes its review.
        e, cls, waiting = st.pick_next(cache, root, ready=[])
        assert e["file_path"] == B and not waiting, e
        # With nothing runnable anywhere the first open file is named, flagged.
        cache[1]["sections"][1]["status"] = " "
        e, cls, waiting = st.pick_next(cache, root, ready=[])
        assert cls == st.NEEDS_WORK and e["file_path"] == A and waiting, e


def test_runnable_sections_skip_a_section_whose_dependencies_are_open():
    with tempfile.TemporaryDirectory() as root:
        cache = corpus(root)
        a, b = cache[0], cache[1]
        _, per_a = st.classify_file(a, root)
        _, per_b = st.classify_file(b, root)
        assert st.runnable_sections(a, per_a, [(A, 2)]) == [2]
        assert st.runnable_sections(a, per_a, []) == []
        assert st.runnable_sections(a, per_a, None) == [1, 2]
        # Shipped and unreviewed: runnable whatever the plan says.
        assert st.runnable_sections(b, per_b, []) == [2]
        # In progress is not shipped: it can still hold implementation work,
        # so it waits for its dependencies like any unbuilt section.
        b["sections"][1]["status"] = "/"
        assert st.runnable_sections(b, per_b, []) == []
        assert st.runnable_sections(b, per_b, [(B, 2)]) == [2]


def plan_stub(rows):
    return f"def ready_rows(root, nodes=None):\n    return {rows!r}\n"


def test_plan_ready_with_no_tool_is_no_answer():
    with tempfile.TemporaryDirectory() as root:
        assert st.plan_ready(root) is None
        write(root, st.PLAN_TOOL, "raise SystemExit(2)\n")
        assert st.plan_ready(root) is None
        write(root, st.PLAN_TOOL, "def ready_rows(root, nodes=None):\n    raise RuntimeError('no plan')\n")
        assert st.plan_ready(root) is None
        write(root, st.PLAN_TOOL, plan_stub([("x", 3), ("bad",), ("y", "4")]))
        assert st.plan_ready(root) == [("x", 3)]


def test_the_planner_gets_the_certified_nodes_not_the_classified_ones():
    # The stub reports what it was handed: the freshly validated nodes, or
    # None (rebuild) when the cache is not fresh; never the oracle's own list.
    with tempfile.TemporaryDirectory() as root:
        write(root, st.PLAN_TOOL, "def ready_rows(root, nodes=None):\n    return [(repr(nodes), 0)]\n")
        real = st._fresh_nodes
        try:
            st._fresh_nodes = lambda r: None
            assert st.plan_ready(root, ["classified"]) == [("None", 0)]
            st._fresh_nodes = lambda r: ["certified"]
            assert st.plan_ready(root, ["classified"]) == [("['certified']", 0)]
        finally:
            st._fresh_nodes = real
        st._LOADED_FROM = None
        assert st._fresh_nodes(root) is None


def test_a_replaced_cache_is_read_again_not_trusted():
    import shutil
    import subprocess
    repo = HERE.parents[2]
    with tempfile.TemporaryDirectory() as root:
        shutil.copytree(repo / "todo", pathlib.Path(root) / "todo")
        shutil.copytree(repo / "scripts" / "todo-graph", pathlib.Path(root) / "scripts" / "todo-graph",
                        ignore=shutil.ignore_patterns("tests", "__pycache__"))
        cache = pathlib.Path(root) / "build" / "todo-cache.json"
        cache.parent.mkdir()

        def build():
            subprocess.run([sys.executable, str(pathlib.Path(root) / "scripts/todo-graph/build.py"), "--quiet",
                            "--root", str(pathlib.Path(root) / "todo"), "--repo-root", root, "--output", str(cache)],
                           check=True, timeout=120)
        build()
        st._LOADED_FROM = str(cache)
        try:
            first = st._fresh_nodes(root)
            assert first is not None
            f = pathlib.Path(root) / "todo/00-infrastructure/TODO-01-developer-tooling-stack.md"
            f.write_text(f.read_text() + "\n")
            assert st._fresh_nodes(root) is None  # edited, not rebuilt: stale
            build()  # the cache file is replaced after the oracle's first load
            assert st._fresh_nodes(root) is not None  # the replacement is read and certified
        finally:
            st._LOADED_FROM = None


def test_a_finished_queue_never_consults_the_plan():
    # A planner that would raise proves it is not called: DONE and BLOCKED
    # come from classification alone, so a slow or broken plan cannot keep a
    # finished run alive.
    with tempfile.TemporaryDirectory() as root:
        write(root, st.PLAN_TOOL, "raise AssertionError('the plan was consulted')\n")
        write(root, A, "# T\n\n## 1. S1\n\n- [x] item\n\n> **Verified:** x\n> **Quality reviewed:** y\n")
        cache = [entry(A, "00-alpha", [(1, "x")])]
        calls = []
        real = st.plan_ready
        st.plan_ready = lambda *a: calls.append(a) or real(*a)
        try:
            e, cls = st.next_file(cache, root)
        finally:
            st.plan_ready = real
        assert e is None and cls is None and not calls, (e, cls, calls)
        # The CLI path too, for a finished and for a blocked queue.
        import contextlib
        import io
        st.plan_ready = lambda *a: calls.append(a) or real(*a)
        try:
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                assert st.cmd_next(cache, root) == 0
            assert json.loads(out.getvalue()) == {"status": "DONE", "file": None}
            write(root, A, "# T\n\n## 1. S1\n\n- [x] item\n\n> **Deferred:** awaiting-hardware\n")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                assert st.cmd_next(cache, root) == 0
            assert json.loads(out.getvalue())["status"] == st.BLOCKED
        finally:
            st.plan_ready = real
        assert not calls, calls


def test_plan_ready_reads_the_real_plan():
    root = str(HERE.parents[2])
    rows = st.plan_ready(root)
    assert rows is not None and rows, "the repository's plan gave no answer"
    assert all(os.path.isfile(os.path.join(root, f)) for f, _ in rows[:20])


def test_the_packet_skips_waiting_and_blocked_sections_end_to_end():
    """cmd_classify on a fixture feeds advance-work's main: section 1 waits on
    a dependency, section 2 is parked (BLOCKED), section 3 is buildable. The
    packet must name 3. Only the subprocess seam is replaced."""
    import contextlib
    import importlib.util
    import io
    spec = importlib.util.spec_from_file_location("advance_work", HERE.parent / "advance-work.py")
    aw = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(aw)
    with tempfile.TemporaryDirectory() as root:
        write(root, A, "# T\n\n## 1. S1\n\n- [ ] item\n\n## 2. S2\n\n- [x] item\n\n"
                       "> **Deferred:** awaiting-hardware\n\n## 3. S3\n\n- [ ] item\n")
        write(root, st.PLAN_TOOL, plan_stub([(A, 3)]))
        cache = [entry(A, "00-alpha", [(1, " "), (2, "x"), (3, " ")])]
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            st.cmd_classify(cache, root, A)
        classified = json.loads(out.getvalue())
        by_n = {s["n"]: s for s in classified["sections"]}
        assert by_n[1].get("waiting") and by_n[2]["class"] == st.BLOCKED and not by_n[3].get("waiting"), by_n

        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            st.cmd_next(cache, root)
        nxt = json.loads(out.getvalue())
        assert nxt["file"] == A and nxt["sections"] == classified["sections"], nxt

        def fake(cmd, cwd, timeout=120):
            if "--next" in cmd:
                return nxt
            assert "--classify" not in cmd, "one advance must not classify twice"
            return None
        aw._json_out = fake
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            aw.main(["--project", root, "--no-pack"])
        packet = json.loads(out.getvalue())
        assert packet["next_section"]["n"] == 3, packet
        # Every open section waiting: the packet names the first, flagged.
        stalled = dict(nxt, sections=[{"n": 1, "class": "NEEDS_WORK", "waiting": True}, {"n": 2, "class": "BLOCKED"}])
        aw._json_out = lambda cmd, cwd, timeout=120: stalled if "--next" in cmd else None
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            aw.main(["--project", root, "--no-pack"])
        packet2 = json.loads(out.getvalue())
        assert packet2["action"] == "work" and packet2["next_section"] == {"n": 1, "class": "NEEDS_WORK", "waiting": True}, packet2
        assert packet["sections_summary"]["waiting"] == 1 and packet["sections_summary"]["deferred"] == 1, packet


if __name__ == "__main__":
    fails = []
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"  PASS {name}")
            except Exception as exc:
                fails.append(name)
                print(f"  FAIL {name}: {exc!r}")
    print(f"triage plan order: {len(fails)} failed, {sum(1 for n in globals() if n.startswith('test_'))} tests")
    sys.exit(1 if fails else 0)
