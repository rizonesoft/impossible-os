#!/usr/bin/env python3
"""A reviewer outage leaves a review owed and the run continues (automation hardening, the outage section).

scripts/overnight/review_owed.py keeps the debt; the triage oracle does not pick an owed section while the
reviewer is down and picks it first when it answers; the phase guard's rollover does not count its unpushed
commits as a failure; the pre-push hook refuses while a debt is open; and an owed section's range is its
recorded base.
"""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPO / "scripts" / "overnight" / "review_owed.py"
sys.path.insert(0, str(REPO / ".claude/hooks"))
sys.path.insert(0, str(REPO / "scripts"))
import review_pin as rp  # noqa: E402
import sequencer_triage as st  # noqa: E402

TODO = "todo/00-x/TODO-01-x.md"
OTHER = "todo/01-y/TODO-01-y.md"
BODY = "# X\n\n## 7. Seven\n\n- [ ] item\n\n## 8. Eight\n\n- [ ] item\n"
STAMPS = "> **Verified:** 2026-11-14 | fixture\n> **Quality reviewed:** 2026-11-14 | fixture\n"


def _git(root, *args):
    done = subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                           "-c", "core.hooksPath=/dev/null", *args], capture_output=True, text=True, timeout=60)
    assert done.returncode == 0, done.stderr
    return done.stdout.strip()


def _repo(tmp_path):
    root = tmp_path / "repo"
    (root / TODO).parent.mkdir(parents=True)
    _git(root, "init", "-q", "-b", "main")
    (root / ".gitignore").write_text(".claude/\n")
    (root / TODO).write_text(BODY)
    _git(root, "add", "-A")
    _git(root, "commit", "-q", "-m", "start")
    return root


def _reviewer(root, down, now=None):
    """Make the reviews manifest say the reviewer is down (two backend failures just now) or answering."""
    reviews = root / ".claude/overnight/reviews"
    reviews.mkdir(parents=True, exist_ok=True)
    now = int(now or time.time())
    lines = []
    for n in range(2):
        log = reviews / f"leg-{n}.out"
        log.write_text(("Codex error: unexpected status 503\nTurn completed (rc=1)\n") if down
                       else "all good\nTurn completed (rc=0)\n")
        lines.append(json.dumps({"ts": now - n, "kind": "plan", "todo": "todo/other.md", "logFile": str(log)}))
    (reviews / "manifest.jsonl").write_text("\n".join(reversed(lines)) + "\n")


def _run(root, *args):
    return subprocess.run([sys.executable, str(SCRIPT), "--root", str(root), *args], capture_output=True, text=True,
                          timeout=60)


def _owed(root):
    path = root / ".claude/state/review-owed.json"
    return json.loads(path.read_text()) if path.exists() else []


# ---- the record ----

def test_a_section_is_recorded_before_it_is_built_only_while_the_reviewer_is_down(tmp_path):
    root = _repo(tmp_path)
    head = _git(root, "rev-parse", "HEAD")
    r = _run(root, "add", "--todo", TODO, "--section", "7")
    assert r.returncode == 2 and "the reviewer answers" in r.stderr, r.stderr
    _reviewer(root, down=False)
    assert _run(root, "probe").returncode == 0
    assert _run(root, "add", "--todo", TODO, "--section", "7").returncode == 2 and not _owed(root)
    _reviewer(root, down=True)
    assert _run(root, "probe").returncode == 75
    r = _run(root, "add", "--todo", TODO, "--section", "7")
    assert r.returncode == 0, r.stderr
    (record,) = _owed(root)
    assert (record["todo"], record["section"], record["base"]) == (TODO, 7, head)


def test_the_first_base_stands_however_late_the_run_asks_again(tmp_path):
    root = _repo(tmp_path)
    head = _git(root, "rev-parse", "HEAD")
    _reviewer(root, down=True)
    assert _run(root, "add", "--todo", TODO, "--section", "7").returncode == 0
    # The build took longer than the outage evidence lasts, the reviewer came back, and the run was interrupted
    # between its commit and anything else: the record was written first, so nothing is lost.
    (root / "src.c").write_text("int a;\n")
    _git(root, "add", "-A")
    _git(root, "commit", "-q", "-m", "build")
    _reviewer(root, down=False)
    assert _run(root, "add", "--todo", TODO, "--section", "7").returncode == 0
    assert [e["base"] for e in _owed(root)] == [head]
    assert _run(root, "list").stdout.strip() == f"{TODO} section 7 base {head}"


def test_a_section_whose_plan_was_reviewed_owes_from_its_plan_pass(tmp_path):
    root = _repo(tmp_path)
    plan = _git(root, "rev-parse", "HEAD")
    (root / "src.c").write_text("int a;\n")
    _git(root, "add", "-A")
    _git(root, "commit", "-q", "-m", "build")
    _reviewer(root, down=True)
    manifest = root / ".claude/overnight/reviews/manifest.jsonl"
    manifest.write_text(json.dumps({"ts": 1, "kind": "plan", "todo": TODO, "section": 7, "head": plan, "rc": 0,
                                    "pass_checked": True, "route": "codex-dispatch.sh"}) + "\n" + manifest.read_text())
    assert _run(root, "add", "--todo", TODO, "--section", "7").returncode == 0
    assert _owed(root)[0]["base"] == plan


def test_refusals_of_add(tmp_path):
    root = _repo(tmp_path)
    _reviewer(root, down=True)
    r = _run(root, "add", "--todo", TODO, "--section", "9")
    assert r.returncode == 2 and "no section 9" in r.stderr
    r = _run(root, "add", "--todo", "todo/none.md", "--section", "7")
    assert r.returncode == 2 and "no section 7" in r.stderr
    (root / TODO).write_text(BODY.replace("## 8.", STAMPS + "\n## 8."))
    r = _run(root, "add", "--todo", TODO, "--section", "7")
    assert r.returncode == 2 and "already stamped" in r.stderr
    assert not _owed(root)


def test_a_debt_is_cleared_only_by_the_sections_stamps_and_the_gate_names_what_is_open(tmp_path):
    root = _repo(tmp_path)
    _reviewer(root, down=True)
    assert _run(root, "gate").returncode == 0
    for n in ("7", "8"):
        assert _run(root, "add", "--todo", TODO, "--section", n).returncode == 0
    r = _run(root, "gate")
    assert r.returncode == 1 and f"{TODO} section 7" in r.stdout and f"{TODO} section 8" in r.stdout
    r = _run(root, "clear", "--todo", TODO, "--section", "7")
    assert r.returncode == 2 and "not stamped in HEAD" in r.stderr
    # One stamp is not both, and a stamp under section 8 is not section 7's.
    (root / TODO).write_text(BODY.replace("## 8.", "> **Verified:** x\n\n## 8.") + "\n" + STAMPS)
    # Stamps that were written but whose commit never landed (the gate refused it) clear nothing.
    assert _run(root, "clear", "--todo", TODO, "--section", "8").returncode == 2
    assert _run(root, "gate").returncode == 1 and len(_owed(root)) == 2
    _git(root, "commit", "-qam", "stamp eight")
    assert _run(root, "clear", "--todo", TODO, "--section", "7").returncode == 2
    assert _run(root, "clear", "--todo", TODO, "--section", "8").returncode == 0
    assert [e["section"] for e in _owed(root)] == [7]
    (root / TODO).write_text(BODY.replace("## 8.", STAMPS + "\n## 8."))
    r = _run(root, "clear", "--todo", TODO, "--section", "7")
    assert r.returncode == 2 and "not stamped in HEAD" in r.stderr
    _git(root, "commit", "-qam", "stamp seven")
    assert _run(root, "clear", "--todo", TODO, "--section", "7").returncode == 0
    assert _owed(root) == [] and _run(root, "gate").returncode == 0
    assert _run(root, "clear", "--todo", TODO, "--section", "7").returncode == 0      # nothing to clear


def test_refusal_a_record_that_cannot_be_read_is_never_read_as_empty(tmp_path):
    root = _repo(tmp_path)
    state = root / ".claude/state/review-owed.json"
    state.parent.mkdir(parents=True)
    for bad in ("{", "{}", '[{"todo": "x"}]', '[{"todo": "x", "section": 7, "base": ""}]'):
        state.write_text(bad)
        for verb in (("gate",), ("list",), ("clear", "--todo", TODO, "--section", "7")):
            r = _run(root, *verb)
            assert r.returncode == 2 and "review-owed.json" in r.stderr, (bad, verb, r.stderr)


# ---- where an owed section starts ----

def test_an_owed_sections_range_is_its_recorded_base_not_its_late_plan_pass(tmp_path):
    root = _repo(tmp_path)
    assert rp.section_base(root, TODO, 7, "planhead") == "planhead"       # nothing owed: the plan pass
    state = root / ".claude/state/review-owed.json"
    state.parent.mkdir(parents=True)
    state.write_text(json.dumps([{"todo": TODO, "section": 7, "base": "before-the-code", "since": 1}]))
    assert rp.section_base(root, TODO, 7, "planhead") == "before-the-code"
    assert rp.section_base(root, TODO, "7", "planhead") == "before-the-code"
    assert rp.section_base(root, TODO, 8, "planhead") == "planhead"
    assert rp.section_base(root, OTHER, 7, "planhead") == "planhead"
    # section_status hands the same base to every caller.
    manifest = root / ".claude/overnight/reviews/manifest.jsonl"
    manifest.parent.mkdir(parents=True)
    manifest.write_text(json.dumps({"kind": "plan", "todo": TODO, "section": 7, "head": "planhead", "rc": 0,
                                    "pass_checked": True}) + "\n")
    assert rp.section_status(root, TODO, 7) == ("passes", "before-the-code")
    assert rp.section_status(root, TODO, 8) == ("new", "")
    state.write_text("{")
    try:
        rp.section_base(root, TODO, 7, "planhead")
    except rp.Refused as exc:
        assert "cannot be read" in str(exc)
    else:
        raise AssertionError("an unreadable record was read as nothing owed")


# ---- the triage oracle ----

def _entry(path, domain, statuses):
    return {"file_path": path, "domain": domain,
            "sections": [{"n": n, "status": s, "deliverable": f"s{n}", "depends_on": []} for n, s in statuses]}


def _corpus(root):
    for path in (TODO, OTHER):
        (root / path).parent.mkdir(parents=True, exist_ok=True)
        (root / path).write_text(BODY)
    return [_entry(TODO, "00-x", [(7, "x"), (8, " ")]), _entry(OTHER, "01-y", [(7, "x"), (8, " ")])]


def _state(root, *owed):
    path = root / ".claude/state/review-owed.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps([{"todo": t, "section": n, "base": "b", "since": i} for i, (t, n) in enumerate(owed)]))
    st._OWED_MEMO.clear()


def _triage(monkeypatch):
    monkeypatch.setattr(st, "OUTAGE_TOOL", str(REPO / "scripts/overnight/codex-outage-check.py"))
    st._OWED_MEMO.clear()


def test_an_owed_section_is_not_picked_while_the_reviewer_is_down(tmp_path, monkeypatch):
    _triage(monkeypatch)
    root = tmp_path
    cache = _corpus(root)
    _reviewer(root, down=True)
    # Nothing owed: the built, unstamped section 7 of the first file is the next work, as always.
    e, cls, waiting = st.pick_next(cache, root, ready=[(TODO, 8), (OTHER, 8)])
    assert e["file_path"] == TODO and not waiting
    _, per = st.classify_file(cache[0], root)
    assert st.runnable_sections(cache[0], per, [(TODO, 8)]) == [7, 8]
    # Owed and the reviewer down: it waits, and the run builds the next section.
    _state(root, (TODO, 7))
    blocked, recovered = st.owed_reviews(root)
    assert blocked == {(TODO, 7)} and recovered == []
    assert st.runnable_sections(cache[0], per, [(TODO, 8)], blocked) == [8]
    assert st.section_list(cache[0], per, [(TODO, 8)], (blocked, []))[0] == \
        {"n": 7, "class": st.NEEDS_WORK, "waiting": True, "review_owed": True}
    # A section that is owed but not built yet is the one being built: it stays runnable.
    cache[0]["sections"][0]["status"] = "/"
    assert st.runnable_sections(cache[0], per, [(TODO, 7), (TODO, 8)], blocked) == [7, 8]


def test_a_file_that_owes_only_reviews_is_passed_over_and_never_named_as_the_fallback(tmp_path, monkeypatch):
    _triage(monkeypatch)
    root = tmp_path
    cache = _corpus(root)
    cache[0]["sections"][1]["status"] = "x"          # both sections of the first file are built
    _reviewer(root, down=True)
    _state(root, (TODO, 7), (TODO, 8))
    e, cls, waiting = st.pick_next(cache, root, ready=[(OTHER, 8)])
    assert e["file_path"] == OTHER, e
    # Nothing buildable anywhere and the other file's built section owed too: nothing can be worked.
    _state(root, (TODO, 7), (TODO, 8), (OTHER, 7))
    e, cls, waiting = st.pick_next(cache, root, ready=[])
    assert e["file_path"] == OTHER and waiting          # its section 8 waits on the plan, and is named
    cache[1]["sections"][1]["status"] = "x"
    _state(root, (TODO, 7), (TODO, 8), (OTHER, 7), (OTHER, 8))
    assert st.pick_next(cache, root, ready=[]) == (None, st.BLOCKED, False)


def test_when_the_reviewer_answers_the_oldest_owed_section_comes_first_across_files(tmp_path, monkeypatch):
    _triage(monkeypatch)
    root = tmp_path
    cache = _corpus(root)
    _reviewer(root, down=False)
    _state(root, (OTHER, 7), (TODO, 7))               # the other file's debt is the older one
    # The run moved on to the first file (its cursor) and the plan offers its next section; the debt still wins.
    (root / st.RUN_STATE).parent.mkdir(parents=True, exist_ok=True)
    (root / st.RUN_STATE).write_text(json.dumps({"active": True, "file": TODO}))
    blocked, recovered = st.owed_reviews(root)
    assert blocked == frozenset() and recovered == [(OTHER, 7), (TODO, 7)]
    e, cls, waiting = st.pick_next(cache, root, ready=[(TODO, 8)])
    assert e["file_path"] == OTHER and cls == st.NEEDS_WORK and not waiting
    # Once it is stamped (no longer NEEDS_WORK) the next debt is the one named.
    (root / OTHER).write_text(BODY.replace("## 8.", STAMPS + "\n## 8."))
    e, _cls, _w = st.pick_next(cache, root, ready=[(TODO, 8)])
    assert e["file_path"] == TODO


def _advance():
    import importlib.util
    spec = importlib.util.spec_from_file_location("advance_work", REPO / "scripts/overnight/advance-work.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_the_work_packet_never_names_an_owed_section_and_names_a_recovered_one_first(tmp_path, monkeypatch):
    _triage(monkeypatch)
    root = tmp_path
    cache = _corpus(root)
    advance = _advance()
    _, per = st.classify_file(cache[0], root)
    # Reviewer down, section 7 owed, section 8 waiting on its dependencies: the file is still named (8 is the
    # stall the worker should see), and the packet's section is 8, never the owed 7.
    _reviewer(root, down=True)
    _state(root, (TODO, 7))
    sections = st.section_list(cache[0], per, [], st.owed_reviews(root))
    assert [s.get("review_owed", False) for s in sections] == [True, False]
    assert advance._next_section(sections)["n"] == 8
    # Every open section owed: the packet names none.
    cache[0]["sections"][1]["status"] = "x"
    _state(root, (TODO, 7), (TODO, 8))
    assert advance._next_section(st.section_list(cache[0], per, [], st.owed_reviews(root))) is None
    # Reviewer back, section 8 the debt, section 7 built and unreviewed too: the debt is the section to work.
    _reviewer(root, down=False)
    _state(root, (TODO, 8))
    sections = st.section_list(cache[0], per, [], st.owed_reviews(root))
    assert [s.get("review_first", False) for s in sections] == [False, True]
    assert advance._next_section(sections)["n"] == 8
    # With nothing owed the packet is what it always was.
    (root / ".claude/state/review-owed.json").unlink()
    st._OWED_MEMO.clear()
    plain = st.section_list(cache[0], per, [], st.owed_reviews(root))
    assert plain == [{"n": 7, "class": st.NEEDS_WORK}, {"n": 8, "class": st.NEEDS_WORK}]
    assert advance._next_section(plain)["n"] == 7


def test_a_record_the_oracle_cannot_read_changes_nothing(tmp_path, monkeypatch):
    _triage(monkeypatch)
    root = tmp_path
    cache = _corpus(root)
    (root / ".claude/state").mkdir(parents=True)
    (root / ".claude/state/review-owed.json").write_text("{")
    st._OWED_MEMO.clear()
    assert st.owed_reviews(root) == (frozenset(), [])
    e, _cls, _w = st.pick_next(cache, root, ready=[])
    assert e["file_path"] == TODO


def test_the_reviewer_is_probed_once_an_invocation(tmp_path, monkeypatch):
    _triage(monkeypatch)
    root = tmp_path
    _corpus(root)
    _reviewer(root, down=True)
    _state(root, (TODO, 7))
    calls = []
    real = subprocess.run
    monkeypatch.setattr(subprocess, "run", lambda *a, **k: (calls.append(a), real(*a, **k))[1])
    for _ in range(4):
        st.owed_reviews(root)
    assert len(calls) == 1


def test_a_probe_that_does_not_answer_in_time_changes_nothing_and_leaves_no_process(tmp_path, monkeypatch):
    root = tmp_path
    cache = _corpus(root)
    _state(root, (TODO, 7))
    slow = tmp_path / "slow-probe.py"
    slow.write_text("import os, sys, time\nopen(sys.argv[2] + '.pid', 'w').write(str(os.getpid()))\ntime.sleep(30)\n")
    monkeypatch.setattr(st, "OUTAGE_TOOL", str(slow))
    monkeypatch.setattr(st, "OUTAGE_MANIFEST", "probe")
    st._OWED_MEMO.clear()
    start = time.monotonic()
    assert st.owed_reviews(root) == (frozenset(), [])
    assert time.monotonic() - start < 4, "the probe outlived the Stop hook's budget"
    pid = int((root / "probe.pid").read_text())
    for _ in range(40):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            break
        time.sleep(0.05)
    else:
        raise AssertionError("the probe process was left running")
    # An unknown is the ordinary order: the built, unstamped section is named as before.
    e, _cls, _w = st.pick_next(cache, root, ready=[])
    assert e["file_path"] == TODO


# ---- the routed reviewer's own passes ----

FAKE_COMPANION = """
const fs = require('fs');
const tail = process.argv.slice(3);
fs.writeFileSync(process.env.FAKE_PROMPT_OUT, tail[0] === '--' ? tail.slice(1).join(' ') : tail.join(' '));
process.stdout.write('# Codex Adversarial Review\\n\\n' + fs.readFileSync(process.env.FAKE_REVIEW, 'utf8'));
"""


def _dispatch(root, tmp_path, kind, base=None):
    pin = rp.load_pin()
    (tmp_path / "companion.js").write_text(FAKE_COMPANION)
    (tmp_path / "review.txt").write_text("\n".join(f"LENS {n}: clean" for n in rp.lenses_for(pin, kind, ()))
                                         + "\nHIGHEST: none\n")
    env = {**os.environ, "CODEX_COMPANION_PATH": str(tmp_path / "companion.js"),
           "CODEX_REVIEWS_DIR": str(root / ".claude/overnight/reviews"),
           "FAKE_PROMPT_OUT": str(tmp_path / "prompt.txt"), "FAKE_REVIEW": str(tmp_path / "review.txt")}
    env.pop("REVIEW_BASE", None)
    if base:
        env["REVIEW_BASE"] = base
    done = subprocess.run(["bash", str(REPO / "scripts/codex-dispatch.sh"), f"[review-kind: {kind}] {TODO} section 7\nx"],
                          capture_output=True, text=True, timeout=120, cwd=str(root), env=env)
    lines = (root / ".claude/overnight/reviews/manifest.jsonl").read_text().splitlines()
    receipts = [json.loads(line) for line in lines]
    return done, [e for e in receipts if e.get("route") == "codex-dispatch.sh"]


def test_every_pass_after_the_plan_is_shown_an_owed_section_from_its_recorded_base(tmp_path):
    root = _repo(tmp_path)
    start = _git(root, "rev-parse", "HEAD")
    _reviewer(root, down=True)
    assert _run(root, "add", "--todo", TODO, "--section", "7").returncode == 0
    (root / "src.c").write_text("int a;\n")
    _git(root, "add", "-A")
    _git(root, "commit", "-q", "-m", "built while the reviewer was down")
    late = _git(root, "rev-parse", "HEAD")
    # The plan pass comes late and is not bound: it reads the plan, not a range.
    done, receipts = _dispatch(root, tmp_path, "plan", base=late)
    assert done.returncode == 0, done.stderr
    # A pass given the late plan's commit, or HEAD, as its base would read nothing of the section.
    for kind in ("round", "performance", "completion"):
        done, _ = _dispatch(root, tmp_path, kind, base=late)
        assert done.returncode == 2 and "starts at " + start[:10] in done.stderr, (kind, done.stderr)
    # With no base given, or the recorded one, it is shown the section, told its range, and the receipt says so.
    for kind, base in (("round", None), ("performance", start), ("completion", None)):
        done, receipts = _dispatch(root, tmp_path, kind, base=base)
        assert done.returncode == 0, (kind, done.stderr)
        assert receipts[-1]["kind"] == kind and receipts[-1]["base"] == start
        assert f"git diff {start} HEAD" in (tmp_path / "prompt.txt").read_text()


# ---- the phase guard's rollover (a frozen file: this change was made attended) ----

def _guard_fixture(tmp_path):
    fx = tmp_path / "fx"
    for d in (".claude/hooks", ".claude/state", "todo/00-infrastructure", "scripts/todo-graph"):
        (fx / d).mkdir(parents=True)
    for hook in ("run_phase_guard.py", "sequencer_triage.py"):
        shutil.copy(REPO / ".claude/hooks" / hook, fx / ".claude/hooks" / hook)
    shutil.copy(REPO / "scripts/todo_fence.py", fx / "scripts/todo_fence.py")
    shutil.copy(REPO / "scripts/todo-graph/cache_schema.py", fx / "scripts/todo-graph/cache_schema.py")
    (fx / "todo/00-infrastructure/TODO-01-fixture.md").write_text(
        "# TODO-01 fixture\n\n> **Validated:** 2026-07-01 | fixture\n\n> **Gap-audited:** 2026-07-02 | fixture\n\n"
        "## 1. Section\n\n- [ ] item\n")
    (fx / ".gitignore").write_text(".claude/state/\n")
    _git(fx, "init", "-q", "-b", "main")
    _git(fx, "add", "-A")
    _git(fx, "commit", "-q", "-m", "init")
    remote = tmp_path / "remote.git"
    subprocess.run(["git", "init", "-q", "--bare", str(remote)], check=True, timeout=60)
    _git(fx, "remote", "add", "origin", str(remote))
    _git(fx, "push", "-q", "-u", "origin", "main")
    (fx / "built.c").write_text("int a;\n")
    _git(fx, "add", "-A")
    _git(fx, "commit", "-q", "-m", "a section built while the reviewer was down")
    return fx


def _rollover(fx):
    env = {k: v for k, v in os.environ.items() if k != "OVERNIGHT_SEQUENCER_RUN"}
    subprocess.run([sys.executable, str(fx / ".claude/hooks/run_phase_guard.py"), "start", "2026-11-14"],
                   capture_output=True, text=True, env=env, cwd=str(fx), timeout=120)
    return subprocess.run([sys.executable, str(fx / ".claude/hooks/run_phase_guard.py"), "rollover"],
                          capture_output=True, text=True, env=env, cwd=str(fx), timeout=600)


def test_an_owed_review_is_not_a_failed_rollover(tmp_path):
    fx = _guard_fixture(tmp_path)
    todo = "todo/00-infrastructure/TODO-01-fixture.md"
    review = fx / ".claude/state/last-codex-review.json"
    # The whole sequence with the real tools. A dispatch failed at the backend and left the review state
    # unreceived; with nothing recorded, the section's unpushed commit and that state both refuse a rollover.
    review.write_text(json.dumps({"received": False, "trigger": "a dispatch that failed at the backend"}))
    r = _rollover(fx)
    assert "1 commit(s) not pushed" in r.stderr and "outstanding Codex review not yet received" in r.stderr, r.stderr
    # The outage is confirmed and the section recorded as owed: recording it is what marks the failed
    # dispatch, whichever commit path the section's code then takes.
    _reviewer(fx, down=True)
    owed = subprocess.run([sys.executable, str(SCRIPT), "--root", str(fx), "add", "--todo", todo, "--section", "1"],
                          capture_output=True, text=True, timeout=60)
    assert owed.returncode == 0, owed.stderr
    assert json.loads(review.read_text()) == {"received": False, "trigger": "a dispatch that failed at the backend",
                                              "outage_debt": True}
    r = _rollover(fx)
    assert "not pushed" not in r.stderr and "outstanding Codex review" not in r.stderr, r.stderr
    # The reviewer comes back and a review is dispatched: a fresh record, with no mark. It is held to the rule
    # until it is received, debt or no debt.
    review.write_text(json.dumps({"received": False}))
    assert "outstanding Codex review not yet received" in _rollover(fx).stderr
    review.write_text(json.dumps({"received": True}))
    assert "outstanding Codex review" not in _rollover(fx).stderr
    # A long outage: the owed section's review is tried again and fails again, which writes a fresh, unmarked
    # record. Asking `add` again keeps the first base and marks that record; with the reviewer answering it
    # marks nothing.
    again = [sys.executable, str(SCRIPT), "--root", str(fx), "add", "--todo", todo, "--section", "1"]
    base = json.loads((fx / ".claude/state/review-owed.json").read_text())[0]["base"]
    review.write_text(json.dumps({"received": False}))
    _reviewer(fx, down=False)
    assert subprocess.run(again, capture_output=True, text=True, timeout=60).returncode == 0
    assert "outage_debt" not in json.loads(review.read_text())
    assert "outstanding Codex review not yet received" in _rollover(fx).stderr
    _reviewer(fx, down=True)
    assert subprocess.run(again, capture_output=True, text=True, timeout=60).returncode == 0
    assert json.loads(review.read_text()) == {"received": False, "outage_debt": True}
    assert json.loads((fx / ".claude/state/review-owed.json").read_text())[0]["base"] == base
    assert "outstanding Codex review" not in _rollover(fx).stderr
    # A received review is never marked, and recording a second debt marks only an unreceived one.
    _git(fx, "commit", "-q", "--allow-empty", "-m", "more work")
    review.write_text(json.dumps({"received": True}))
    (fx / todo).write_text((fx / todo).read_text() + "\n## 2. Next\n\n- [ ] item\n")
    assert subprocess.run([sys.executable, str(SCRIPT), "--root", str(fx), "add", "--todo", todo, "--section", "2"],
                          capture_output=True, text=True, timeout=60).returncode == 0
    assert "outage_debt" not in json.loads(review.read_text())
    # An empty record, or one that cannot be read, is no debt: the ordinary refusals stand, mark or no mark.
    review.write_text(json.dumps({"received": False, "outage_debt": True}))
    for text in ("[]", "{"):
        (fx / ".claude/state/review-owed.json").write_text(text)
        r = _rollover(fx)
        assert "commit(s) not pushed" in r.stderr and "outstanding Codex review not yet received" in r.stderr


# ---- the pre-push hook ----

def test_a_push_is_refused_while_a_review_is_owed(tmp_path):
    fx = tmp_path / "fx"
    for d in (".githooks/lib", "scripts/overnight", "scripts/todo-graph", TODO.rsplit("/", 1)[0]):
        (fx / d).mkdir(parents=True)
    shutil.copy(REPO / ".githooks/pre-push", fx / ".githooks/pre-push")
    shutil.copy(REPO / ".githooks/lib/isolate-git-env.sh", fx / ".githooks/lib/isolate-git-env.sh")
    for rel in ("scripts/overnight/review_owed.py", "scripts/overnight/codex-outage-check.py", "scripts/todo_fence.py",
                "scripts/todo-graph/cache_schema.py"):
        shutil.copy(REPO / rel, fx / rel)
    (fx / TODO).write_text(BODY)
    (fx / ".gitignore").write_text(".claude/\n")
    _git(fx, "init", "-q", "-b", "main")
    _git(fx, "add", "-A")
    _git(fx, "commit", "-q", "-m", "start")
    _reviewer(fx, down=True)
    owed = [sys.executable, str(fx / "scripts/overnight/review_owed.py"), "--root", str(fx)]
    assert subprocess.run([*owed, "add", "--todo", TODO, "--section", "7"], capture_output=True, timeout=60).returncode == 0

    def push():
        return subprocess.run(["bash", str(fx / ".githooks/pre-push"), "origin", "x"], input="", capture_output=True,
                              text=True, cwd=str(fx), timeout=120)

    r = push()
    assert r.returncode == 1, r.stdout + r.stderr
    assert "a review is owed" in r.stdout and f"{TODO} section 7" in r.stdout
    # A record that cannot be read refuses as well.
    state = fx / ".claude/state/review-owed.json"
    state.write_text("{")
    r = push()
    assert r.returncode == 1 and "a review is owed" in r.stdout
    # Stamped and cleared, the hook's own question is answered: nothing is owed.
    state.write_text(json.dumps([{"todo": TODO, "section": 7, "base": "b", "since": 1}]))
    (fx / TODO).write_text(BODY.replace("## 8.", STAMPS + "\n## 8."))
    _git(fx, "commit", "-qam", "stamp seven")
    assert subprocess.run([*owed, "clear", "--todo", TODO, "--section", "7"], capture_output=True, timeout=60).returncode == 0
    assert subprocess.run([*owed, "gate"], capture_output=True, timeout=60).returncode == 0
    # The check sits before every costly gate of the hook.
    text = (REPO / ".githooks/pre-push").read_text()
    assert text.index("review_owed.py") < text.index("# Files changed across every non-delete ref update")
