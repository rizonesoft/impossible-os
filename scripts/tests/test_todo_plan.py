#!/usr/bin/env python3
"""Tests for scripts/todo-plan.py against a small fixture corpus.

The fixture is a real todo/ tree in a temporary directory: the tool builds its
own graph from it with the repository's builder and classifier, so these tests
cover the whole path a real sync takes.
"""
import importlib.util
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parent / "todo-plan.py"
PLAN = "todo/implementation-plan.md"
S = "§"

STAMPS = ("> **Verified:** 2026-01-01 | commit `abc1234` | 1/1 items | build OK\n"
          "> **Quality reviewed:** 2026-01-01 | Codex 3x | 0 open\n")


def todo(ident, domain, title, rows, bodies):
    """rows: (n, deliverable, depends, status); bodies: n -> (heading, extra text)."""
    out = ["---", "schema_version: 1", f"id: {ident}", f"domain: {domain}", "status: active",
           f'title: "{title}"', "---", "", f"# {title}", "", "## Implementation Order", "",
           "| Order | Deliverable | Depends On | Status |", "| :---: | --- | --- | :---: |"]
    out += [f"| {n} | {d} | {dep} | [{st}] |" for n, d, dep, st in rows]
    for n, (heading, extra) in bodies.items():
        out += ["", f"## {n}. {heading}", "", "- [ ] one item", extra]
    return "\n".join(out) + "\n"


def make_repo(tmp, beta_rows=None, beta_bodies=None):
    root = pathlib.Path(tmp)
    (root / "todo" / "00-alpha").mkdir(parents=True)
    (root / "todo" / "01-beta").mkdir(parents=True)
    # Alpha 2 waits on Beta 1, a later domain: it must be planned after it.
    (root / "todo" / "00-alpha" / "TODO-01-alpha.md").write_text(todo(
        "alpha", "00-alpha", "TODO-01 -- Alpha",
        [(1, "First", "--", "x"), (2, "Second", "TODO-01-beta " + S + "1", " "), (3, "Third", S + "1", " ")],
        {1: ("First Thing", STAMPS), 2: ("Second Thing", ""), 3: ("Third Thing", "")}))
    (root / "todo" / "01-beta" / "TODO-01-beta.md").write_text(todo(
        "beta", "01-beta", "TODO-01 -- Beta",
        beta_rows or [(1, "Base", "--", " "), (2, "Top", S + "1", " ")],
        beta_bodies or {1: ("Base Layer", ""), 2: ("Top Layer", "")}))
    return root


def run(root, *args):
    return subprocess.run([sys.executable, str(SCRIPT), "--repo-root", str(root), *args],
                          text=True, capture_output=True, timeout=300)


def rows(root):
    return [ln for ln in (root / PLAN).read_text().splitlines() if ln.startswith("| [")]


def index_of(lines, ref):
    hits = [i for i, ln in enumerate(lines) if f"`{ref}`" in ln]
    assert len(hits) == 1, (ref, lines)
    return hits[0]


def test_seed_lists_every_section_once_in_dependency_order():
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d)
        r = run(root, "--seed")
        assert r.returncode == 0, r.stderr + r.stdout
        lines = rows(root)
        assert len(lines) == 5, lines
        assert "[x]" in lines[index_of(lines, f"D00 T01 {S}1")]
        assert "First Thing" in lines[index_of(lines, f"D00 T01 {S}1")]
        assert index_of(lines, f"D01 T01 {S}1") < index_of(lines, f"D00 T01 {S}2")
        text = (root / PLAN).read_text()
        assert "**1 of 5 sections complete (20%).**" in text, text
        # The dependent sits in the later domain's phase, not its own.
        assert text.index("### Phase 1 --") < text.index(f"`D00 T01 {S}2`")
        assert run(root, "--check").returncode == 0
        assert run(root, "--seed").returncode == 2  # never overwrites a plan


def test_check_fails_on_a_stale_box_and_sync_repairs_it():
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d)
        assert run(root, "--seed").returncode == 0
        beta = root / "todo" / "01-beta" / "TODO-01-beta.md"
        beta.write_text(beta.read_text().replace("| 1 | Base | -- | [ ] |", "| 1 | Base | -- | [x] |")
                        .replace("## 1. Base Layer\n\n- [ ] one item\n", "## 1. Base Layer\n\n- [x] one item\n" + STAMPS))
        r = run(root, "--check")
        assert r.returncode == 1 and "is stale" in r.stderr, r.stderr
        assert run(root, "--sync").returncode == 0
        assert "[x]" in rows(root)[index_of(rows(root), f"D01 T01 {S}1")]
        assert "**2 of 5 sections complete (40%).**" in (root / PLAN).read_text()
        assert run(root, "--check").returncode == 0


def test_shipped_without_both_stamps_is_not_done():
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d, beta_rows=[(1, "Base", "--", "x"), (2, "Top", S + "1", " ")])
        assert run(root, "--seed").returncode == 0
        assert "[/]" in rows(root)[index_of(rows(root), f"D01 T01 {S}1")]


def test_sync_places_a_new_section_after_its_file_and_keeps_hand_written_prose():
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d)
        assert run(root, "--seed").returncode == 0
        plan = root / PLAN
        plan.write_text(plan.read_text().replace(
            "Seeded from the `01` domain and from the sections of earlier domains that depend on it.",
            "Beta comes second because Alpha needs its base layer."))
        beta = root / "todo" / "01-beta" / "TODO-01-beta.md"
        beta.write_text(todo("beta", "01-beta", "TODO-01 -- Beta",
                             [(1, "Base", "--", " "), (2, "Top", S + "1", " "), (3, "New", S + "2", " ")],
                             {1: ("Base Layer", ""), 2: ("Top Layer", ""), 3: ("Newly Filed", "")}))
        assert run(root, "--check").returncode == 1
        r = run(root, "--sync")
        assert r.returncode == 0 and f"placed D01 T01 {S}3 in Phase 1" in r.stdout, r.stdout + r.stderr
        lines = rows(root)
        assert index_of(lines, f"D01 T01 {S}3") == index_of(lines, f"D01 T01 {S}2") + 1
        assert "Beta comes second because Alpha needs its base layer." in plan.read_text()
        assert run(root, "--check").returncode == 0


def test_sync_removes_a_row_whose_section_is_gone_and_a_duplicate():
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d)
        assert run(root, "--seed").returncode == 0
        plan = root / PLAN
        dup = rows(root)[index_of(rows(root), f"D01 T01 {S}2")]
        plan.write_text(plan.read_text() + dup + f"\n| [ ] | `D01 T09 {S}7` | Gone |\n")
        r = run(root, "--sync")
        assert r.returncode == 0, r.stderr
        assert "no such section" in r.stdout and "duplicate" in r.stdout, r.stdout
        assert len(rows(root)) == 5
        assert run(root, "--check").returncode == 0


def test_check_refuses_a_row_moved_before_its_dependency():
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d)
        assert run(root, "--seed").returncode == 0
        plan = root / PLAN
        lines = plan.read_text().split("\n")
        a = next(i for i, ln in enumerate(lines) if f"`D01 T01 {S}1`" in ln)
        b = next(i for i, ln in enumerate(lines) if f"`D01 T01 {S}2`" in ln)
        lines[a], lines[b] = lines[b], lines[a]
        plan.write_text("\n".join(lines))
        r = run(root, "--check")
        assert r.returncode == 1, r.stdout
        assert f"D01 T01 {S}2 (Phase 1) sits before its dependency D01 T01 {S}1" in r.stderr, r.stderr
        # A sync does not reorder rows: the order is authored, the check holds it.
        assert run(root, "--sync").returncode == 1


def test_next_lists_only_what_can_be_built_now():
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d)
        assert run(root, "--seed").returncode == 0
        r = run(root, "--next")
        assert r.returncode == 0, r.stderr
        got = [ln.split("`")[1] for ln in r.stdout.splitlines()]
        # Alpha 3 waits only on finished work; Beta 1 waits on nothing.
        assert got == [f"D01 T01 {S}1", f"D00 T01 {S}3"] or got == [f"D00 T01 {S}3", f"D01 T01 {S}1"], got
        assert got[0] == [ln.split("`")[1] for ln in rows(root) if ln.split("`")[1] in got][0]


def test_long_headings_become_short_row_titles():
    long_name = "Panic QR Code *(deferred -- needs a module diff and a phone scan; the loader encoder is the seed)*"
    wordy = "A Heading That Simply Runs On And On Past Any Width A Table Cell Could Reasonably Be Asked To Hold, And Then Carries On For Some While Longer Until Nobody Is Reading `tail`"
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d, beta_bodies={1: (long_name, ""), 2: (wordy + " `[Opus]`", "")})
        assert run(root, "--seed").returncode == 0
        lines = rows(root)
        assert lines[index_of(lines, f"D01 T01 {S}1")].split("|")[3].strip() == "Panic QR Code", lines
        cell = lines[index_of(lines, f"D01 T01 {S}2")].split("|")[3].strip()
        assert cell.endswith("...") and len(cell) <= 120 and "[Opus]" not in cell and "`" not in cell, cell
        assert run(root, "--check").returncode == 0


def test_resolve_reads_a_pasted_row():
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d)
        assert run(root, "--seed").returncode == 0
        r = run(root, "--resolve", f"process: | [ ] | `D00 T01 {S}2`  | Second Thing |")
        assert r.returncode == 0, r.stderr
        assert r.stdout.split("\t")[:2] == ["todo/00-alpha/TODO-01-alpha.md", "2"], r.stdout
        assert run(root, "--resolve", f"D07 T01 {S}1").returncode == 1


def test_a_cycle_is_listed_and_fails_the_check():
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d, beta_rows=[(1, "Base", S + "2", " "), (2, "Top", S + "1", " ")])
        assert run(root, "--seed").returncode == 0
        r = run(root, "--check")
        assert r.returncode == 1 and f"dependency cycle: D01 T01 {S}1, D01 T01 {S}2" in r.stderr, r.stderr
        r = run(root, "--cycles")
        assert r.returncode == 0 and len(r.stdout.splitlines()) == 2, r.stdout
        # Neither member of the cycle can be built until the tables are corrected.
        assert f"D01 T01 {S}" not in run(root, "--next").stdout
        # Breaking the cycle in the table clears the check.
        beta = root / "todo" / "01-beta" / "TODO-01-beta.md"
        beta.write_text(beta.read_text().replace(f"| 1 | Base | {S}2 |", "| 1 | Base | -- |"))
        assert run(root, "--sync").returncode == 0
        r = run(root, "--check")
        assert r.returncode == 0, r.stderr


def test_a_row_labelled_with_another_section_fails_the_check():
    # Order 1 describes section 2: without a Section column the graph reads
    # Order as the section number, so every "Depends On" cell naming either
    # section resolves to the wrong one.
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d, beta_rows=[(1, S + "2 Top", "--", " "), (2, S + "1 Base", "--", " ")])
        assert run(root, "--seed").returncode == 0
        r = run(root, "--check")
        assert r.returncode == 1 and f"todo/01-beta/TODO-01-beta.md Order 1 is labelled {S}2" in r.stderr, r.stderr
        # A matching label is fine.
        root2 = make_repo(d + "/ok", beta_rows=[(1, S + "1 Base", "--", " "), (2, S + "2 Top", S + "1", " ")])
        assert run(root2, "--seed").returncode == 0
        assert run(root2, "--check").returncode == 0, run(root2, "--check").stderr


def test_a_section_that_depends_on_itself_fails_the_check():
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d, beta_rows=[(1, S + "1 Base", "--", " "), (2, S + "2 Top", S + "2", " ")])
        assert run(root, "--seed").returncode == 0
        r = run(root, "--check")
        assert r.returncode == 1 and "depends on itself" in r.stderr, r.stderr


def test_a_dependency_on_a_missing_section_fails_the_check():
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d, beta_rows=[(1, S + "1 Base", "--", " "), (2, S + "2 Top", S + "9", " ")])
        assert run(root, "--seed").returncode == 0
        r = run(root, "--check")
        assert r.returncode == 1 and "names no section that work can finish" in r.stderr, r.stderr


def test_a_bare_reference_to_the_own_file_is_a_self_dependency():
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d, beta_rows=[(1, S + "1 Base", "--", "x"), (2, S + "2 Top", "TODO-01", " ")])
        assert run(root, "--seed").returncode == 0
        r = run(root, "--check")
        assert r.returncode == 1 and "depends on itself" in r.stderr, r.stderr


def test_an_edge_between_two_cycles_is_still_held():
    # Beta 1 and 2 form one cycle, Beta 3 and 4 another, and 1 also waits on 3.
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d, beta_rows=[(1, "A", S + "2, " + S + "3", " "), (2, "B", S + "1", " "),
                                       (3, "C", S + "4", " "), (4, "D", S + "3", " ")],
                         beta_bodies={1: ("A", ""), 2: ("B", ""), 3: ("C", ""), 4: ("D", "")})
        assert run(root, "--seed").returncode == 0
        lines = rows(root)
        assert index_of(lines, f"D01 T01 {S}3") < index_of(lines, f"D01 T01 {S}1"), lines
        r = run(root, "--check")
        assert "sits before" not in r.stderr and r.stderr.count("dependency cycle:") == 2, r.stderr
        plan = root / PLAN
        text = plan.read_text().split("\n")
        a = next(i for i, ln in enumerate(text) if f"`D01 T01 {S}1`" in ln)
        c = next(i for i, ln in enumerate(text) if f"`D01 T01 {S}3`" in ln)
        text[a], text[c] = text[c], text[a]
        plan.write_text("\n".join(text))
        r = run(root, "--check")
        assert r.returncode == 1 and f"D01 T01 {S}1 (Phase 1) sits before its dependency D01 T01 {S}3" in r.stderr, r.stderr


def git(root, *args):
    done = subprocess.run(["git", "-C", str(root), "-c", "user.name=plan-fixture", "-c", "user.email=plan-fixture@invalid",
                           "-c", "core.hooksPath=/dev/null", *args], text=True, capture_output=True, timeout=120)
    assert done.returncode == 0, done.stderr
    return done.stdout


def ship_beta_1(root):
    beta = root / "todo" / "01-beta" / "TODO-01-beta.md"
    beta.write_text(beta.read_text().replace("| 1 | Base | -- | [ ] |", "| 1 | Base | -- | [x] |")
                    .replace("## 1. Base Layer\n\n- [ ] one item\n", "## 1. Base Layer\n\n- [x] one item\n" + STAMPS))


def test_staged_check_judges_the_index_not_the_working_tree():
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d)
        assert run(root, "--seed").returncode == 0
        git(root, "init", "-q")
        git(root, "add", "-A")
        git(root, "commit", "-q", "-m", "fixture")
        assert run(root, "--check", "--staged").returncode == 0
        # A TODO edit outside the commit: the working tree is stale, the index is not.
        ship_beta_1(root)
        assert run(root, "--check").returncode == 1
        assert run(root, "--check", "--staged").returncode == 0
        # The TODO edit staged, the plan synced but NOT staged: the commit would carry a stale plan.
        git(root, "add", "todo/01-beta")
        assert run(root, "--sync").returncode == 0
        assert run(root, "--check").returncode == 0
        r = run(root, "--check", "--staged")
        assert r.returncode == 1 and "as staged" in r.stderr, r.stderr
        git(root, "add", PLAN)
        assert run(root, "--check", "--staged").returncode == 0
        # A stale plan in the index is still judged when the working tree no longer has the file.
        git(root, "commit", "-q", "-m", "shipped")
        alpha = root / "todo" / "00-alpha" / "TODO-01-alpha.md"
        alpha.write_text(alpha.read_text().replace("## 3. Third Thing", "## 3. Renamed Thing"))
        git(root, "add", "todo/00-alpha")
        (root / PLAN).unlink()
        assert run(root, "--check", "--staged").returncode == 1


def test_a_second_sync_waits_for_the_first_and_gives_up_by_name():
    import fcntl
    import os
    with tempfile.TemporaryDirectory() as d:
        root = make_repo(d)
        assert run(root, "--seed").returncode == 0
        before = (root / PLAN).read_text()
        ship_beta_1(root)
        with open(root / "build" / ".todo-plan.lock", "w") as held:
            fcntl.flock(held, fcntl.LOCK_EX)
            r = subprocess.run([sys.executable, str(SCRIPT), "--repo-root", str(root), "--sync"], text=True,
                               capture_output=True, timeout=300, env={**os.environ, "TODO_PLAN_LOCK_WAIT_S": "0.3"})
            assert r.returncode == 2 and "another sync holds" in r.stderr, r.stderr
            assert (root / PLAN).read_text() == before
        assert run(root, "--sync").returncode == 0
        assert (root / PLAN).read_text() != before


def test_a_plan_that_changed_under_the_sync_is_not_overwritten():
    spec = importlib.util.spec_from_file_location("todo_plan", SCRIPT)
    tp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tp)
    with tempfile.TemporaryDirectory() as d:
        path = pathlib.Path(d) / "implementation-plan.md"
        path.write_text("edited by someone else\n")
        try:
            tp.write_atomic(path, "ours\n", "what we read\n")
        except tp.PlanError:
            pass
        else:
            raise AssertionError("a changed plan was overwritten")
        assert path.read_text() == "edited by someone else\n"
        assert [p.name for p in pathlib.Path(d).iterdir()] == ["implementation-plan.md"]
        tp.write_atomic(path, "ours\n", "edited by someone else\n")
        assert path.read_text() == "ours\n"


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
    print(f"PASS: todo-plan ({len(tests)} tests)")
